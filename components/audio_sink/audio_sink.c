/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * PCM output path: a ring buffer filled from Bluetooth stack context and
 * drained by a dedicated task that writes to I2S.
 *
 * The split matters. The A2DP data callback runs inside Bluedroid; anything
 * slow there stalls the link and shows up as dropouts. So the callback only
 * ever does a non-blocking ring buffer send, and every cost -- the blocking
 * I2S write, and later the DSP -- lives on the writer task, pinned to the
 * core the radio is not using.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_sink.h"

static const char *TAG = "audio_sink";

#define RINGBUF_BYTES      (CONFIG_PUCK_AUDIO_RINGBUF_KB * 1024)
#define PREFETCH_BYTES     ((RINGBUF_BYTES / 100) * CONFIG_PUCK_AUDIO_PREFETCH_PERCENT)

/* The default I2S DMA chain is dma_desc_num(6) x dma_frame_num(240) = 1440
 * frames = 5760 bytes at 16-bit stereo. A write of a quarter of that keeps the
 * descriptors fed while bounding how long a single write can block: one
 * descriptor drains in 240/44100 = 5.44 ms. */
#define WRITE_CHUNK_BYTES  1440

/* How long a write may wait for a free DMA descriptor.
 *
 * NOT portMAX_DELAY. i2s_channel_write() takes MILLISECONDS and feeds them to
 * pdMS_TO_TICKS, which is 32-bit: portMAX_DELAY overflows to roughly twelve
 * hours. On a disabled channel the write blocks on a semaphore that only
 * i2s_channel_enable() ever gives, so a writer that entered the call just
 * after a stop would wedge for half a day holding a ring buffer item. The IDF
 * example has exactly this bug. 100 ms is ~18x the descriptor period, so
 * normal playback never reaches it. */
#define WRITE_TIMEOUT_MS   100

/* Interleaved 16-bit stereo frames are 4 bytes. If either of these stopped
 * being a multiple of 4, a ring buffer wrap could hand back a frame-straddling
 * block and the DSP would silently swap left and right for the rest of the
 * stream. */
_Static_assert(WRITE_CHUNK_BYTES % 4 == 0, "chunk must hold whole stereo frames");
_Static_assert((CONFIG_PUCK_AUDIO_RINGBUF_KB * 1024) % 4 == 0,
               "ring buffer must hold whole stereo frames");

#define WRITER_TASK_STACK  3072
/* Just below the Bluetooth stack task, so audio is served promptly but never
 * starves the radio. */
#define WRITER_TASK_PRIO   (configMAX_PRIORITIES - 3)

typedef enum {
    RB_PREFETCHING,  /*!< filling the cushion; writer is parked */
    RB_PLAYING,      /*!< cushion is full; writer is draining */
    RB_DROPPING,     /*!< buffer overflowed; shed payloads until it drains */
} ringbuf_mode_t;

/* Q15 fixed point: 32768 is unity gain. */
#define GAIN_UNITY  32768

typedef struct {
    i2s_chan_handle_t       tx;
    RingbufHandle_t         ringbuf;
    TaskHandle_t            writer;
    SemaphoreHandle_t       parked;    /*!< given while the writer holds nothing */
    volatile bool           running;   /*!< I2S channel enabled */
    volatile ringbuf_mode_t mode;
    volatile int32_t        gain_q15;  /*!< playback gain, GAIN_UNITY == 0 dB */
    audio_sink_process_cb_t processor; /*!< optional DSP hook, writer task only */
    uint32_t                sample_rate_hz;
    uint8_t                 channels;
    int64_t                 last_drop_log_us;
    audio_sink_stats_t      stats;
} audio_sink_t;

static audio_sink_t s_snk;

/**
 * Scale a block of interleaved 16-bit samples in place.
 *
 * In place is deliberate: the block is our own ring buffer memory and is
 * consumed immediately afterwards, so a second buffer would only cost RAM and
 * a copy. An odd trailing byte cannot form a sample and is left alone.
 */
static void apply_gain(uint8_t *block, size_t bytes, int32_t gain_q15)
{
    int16_t *samples = (int16_t *)block;
    const size_t count = bytes / sizeof(int16_t);

    for (size_t i = 0; i < count; i++) {
        /* Inputs are 16-bit and the gain never exceeds unity, so the product
         * always fits in int32 and cannot clip. */
        samples[i] = (int16_t)(((int32_t)samples[i] * gain_q15) >> 15);
    }
}

static i2s_std_slot_config_t slot_config_for(uint8_t channels)
{
    const i2s_slot_mode_t mode = (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
#if CONFIG_PUCK_I2S_FORMAT_MSB
    i2s_std_slot_config_t slot = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, mode);
#else
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, mode);
#endif
    return slot;
}

static i2s_std_clk_config_t clk_config_for(uint32_t sample_rate_hz)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
#if CONFIG_PUCK_I2S_USE_APLL
    clk.clk_src = I2S_CLK_SRC_APLL;
#endif
    return clk;
}

static void writer_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Park while stopped or while the cushion refills. Waking on a
         * notification costs less than polling an idle ring buffer.
         *
         * The parked semaphore is what lets audio_sink_stop() know the writer
         * holds no ring buffer item, so it can safely disable the channel and
         * flush. The timeout is a backstop: every state that should run has a
         * notification, but a missed one would otherwise be permanent silence
         * rather than a 100 ms hiccup. */
        if (!s_snk.running || s_snk.mode == RB_PREFETCHING) {
            xSemaphoreGive(s_snk.parked);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            xSemaphoreTake(s_snk.parked, 0);
            continue;
        }

        size_t chunk = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_snk.ringbuf, &chunk,
                                                          pdMS_TO_TICKS(20),
                                                          WRITE_CHUNK_BYTES);
        if (data == NULL || chunk == 0) {
            /* Ran dry. Rebuild the cushion instead of dribbling samples out,
             * which sounds far worse than a brief gap. */
            if (s_snk.running) {
                s_snk.stats.underruns++;
                s_snk.mode = RB_PREFETCHING;
                ESP_LOGD(TAG, "underrun, re-prefetching");
            }
            continue;
        }

        /* Processing order is EQ then volume: the equaliser works at full
         * scale where it has the most headroom, and attenuation comes last. */
        const audio_sink_process_cb_t processor = s_snk.processor;
        if (processor != NULL) {
            /* Read once: a format change mid-block would otherwise swap which
             * filter state each sample belongs to, halfway through. */
            const uint8_t channels = s_snk.channels;
            processor((int16_t *)data, chunk / sizeof(int16_t), channels);
        }

        /* Read once: the AVRCP task can change this mid-block, and a torn
         * gain would put a step in the middle of the waveform. */
        const int32_t gain = s_snk.gain_q15;
        if (gain != GAIN_UNITY) {
            apply_gain(data, chunk, gain);
        }

        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_snk.tx, data, chunk, &written, WRITE_TIMEOUT_MS);
        vRingbufferReturnItem(s_snk.ringbuf, data);
        if (err != ESP_OK && s_snk.running) {
            /* A timeout or a disabled channel while stopping is expected; the
             * loop re-reads the state on the next pass either way. */
            ESP_LOGD(TAG, "i2s write returned %s (%u of %u bytes)",
                     esp_err_to_name(err), (unsigned)written, (unsigned)chunk);
        }
    }
}

esp_err_t audio_sink_init(void)
{
    ESP_RETURN_ON_FALSE(s_snk.tx == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    esp_err_t ret = ESP_OK;   /* ESP_GOTO_ON_* report through this */

    memset(&s_snk, 0, sizeof(s_snk));
    s_snk.sample_rate_hz = 44100;
    s_snk.channels = 2;
    s_snk.mode = RB_PREFETCHING;
    s_snk.gain_q15 = GAIN_UNITY;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Emit silence rather than the last DMA contents if the buffer starves. */
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_snk.tx, NULL), TAG,
                        "i2s channel alloc failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = clk_config_for(s_snk.sample_rate_hz),
        .slot_cfg = slot_config_for(s_snk.channels),
        .gpio_cfg = {
            /* No master clock wire. The PCM5102A derives its own from BCK
             * once the SCK bridge on the board is soldered, which is what the
             * direct-solder layout does. */
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_PUCK_I2S_BCK_GPIO,
            .ws   = CONFIG_PUCK_I2S_LRCK_GPIO,
            .dout = CONFIG_PUCK_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_snk.tx, &std_cfg), err, TAG,
                      "i2s std init failed");

    s_snk.ringbuf = xRingbufferCreate(RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    ESP_GOTO_ON_FALSE(s_snk.ringbuf != NULL, ESP_ERR_NO_MEM, err, TAG,
                      "ring buffer alloc failed (%d bytes)", RINGBUF_BYTES);

    s_snk.parked = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(s_snk.parked != NULL, ESP_ERR_NO_MEM, err, TAG,
                      "parked semaphore alloc failed");

    ESP_GOTO_ON_FALSE(xTaskCreatePinnedToCore(writer_task, "i2s_writer", WRITER_TASK_STACK,
                                              NULL, WRITER_TASK_PRIO, &s_snk.writer,
                                              CONFIG_PUCK_AUDIO_WRITER_CORE) == pdPASS,
                      ESP_ERR_NO_MEM, err, TAG, "writer task create failed");

    ESP_LOGI(TAG, "ready: bck=%d ws=%d dout=%d, %d kB buffer, prefetch %d%%",
             CONFIG_PUCK_I2S_BCK_GPIO, CONFIG_PUCK_I2S_LRCK_GPIO, CONFIG_PUCK_I2S_DOUT_GPIO,
             CONFIG_PUCK_AUDIO_RINGBUF_KB, CONFIG_PUCK_AUDIO_PREFETCH_PERCENT);
    return ESP_OK;

err:
    audio_sink_deinit();
    return ret;
}

void audio_sink_deinit(void)
{
    if (s_snk.writer != NULL) {
        vTaskDelete(s_snk.writer);
        s_snk.writer = NULL;
    }
    if (s_snk.tx != NULL) {
        if (s_snk.running) {
            i2s_channel_disable(s_snk.tx);
            s_snk.running = false;
        }
        i2s_del_channel(s_snk.tx);
        s_snk.tx = NULL;
    }
    if (s_snk.ringbuf != NULL) {
        vRingbufferDelete(s_snk.ringbuf);
        s_snk.ringbuf = NULL;
    }
    if (s_snk.parked != NULL) {
        vSemaphoreDelete(s_snk.parked);
        s_snk.parked = NULL;
    }
}

esp_err_t audio_sink_start(void)
{
    ESP_RETURN_ON_FALSE(s_snk.tx != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (s_snk.running) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_snk.tx), TAG, "i2s enable failed");
    s_snk.running = true;
    s_snk.mode = RB_PREFETCHING;
    /* Wake the writer even though it will find the buffer empty and park
     * again: without this, the states reachable from here have no wakeup
     * source at all if the buffer is already above the prefetch level. */
    xTaskNotifyGive(s_snk.writer);
    ESP_LOGI(TAG, "output started at %" PRIu32 " Hz, %u ch",
             s_snk.sample_rate_hz, s_snk.channels);
    return ESP_OK;
}

esp_err_t audio_sink_stop(void)
{
    ESP_RETURN_ON_FALSE(s_snk.tx != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (!s_snk.running) {
        return ESP_OK;
    }

    s_snk.running = false;
    s_snk.mode = RB_PREFETCHING;
    xTaskNotifyGive(s_snk.writer);

    /* Wait for the writer to reach its park point before touching the channel
     * or the buffer. It bounds at WRITE_TIMEOUT_MS plus a pass of the loop. */
    if (xSemaphoreTake(s_snk.parked, pdMS_TO_TICKS(WRITE_TIMEOUT_MS + 100)) != pdTRUE) {
        ESP_LOGW(TAG, "writer did not park in time; flush skipped");
    } else {
        xSemaphoreGive(s_snk.parked);
    }

    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_snk.tx), TAG, "i2s disable failed");

    /* Stale audio from the previous stream would otherwise play on resume --
     * at the new rate, if the stop was for a format change.
     *
     * This only works because the writer is parked. A byte-mode ring buffer
     * refuses a second retrieval while one is outstanding, so doing this while
     * the writer held an item failed silently and flushed nothing. */
    size_t stale = 0;
    void *item = NULL;
    while ((item = xRingbufferReceiveUpTo(s_snk.ringbuf, &stale, 0, WRITE_CHUNK_BYTES)) != NULL) {
        vRingbufferReturnItem(s_snk.ringbuf, item);
    }

    ESP_LOGI(TAG, "output stopped (packets=%" PRIu32 " dropped=%" PRIu32 " underruns=%" PRIu32 ")",
             s_snk.stats.packets, s_snk.stats.dropped, s_snk.stats.underruns);
    return ESP_OK;
}

esp_err_t audio_sink_set_format(uint32_t sample_rate_hz, uint8_t channels)
{
    ESP_RETURN_ON_FALSE(s_snk.tx != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(sample_rate_hz > 0 && (channels == 1 || channels == 2),
                        ESP_ERR_INVALID_ARG, TAG, "bad format %" PRIu32 " Hz / %u ch",
                        sample_rate_hz, channels);

    if (sample_rate_hz == s_snk.sample_rate_hz && channels == s_snk.channels) {
        return ESP_OK;
    }

    /* i2s_channel_reconfig_* require the channel to be disabled. The IDF
     * example omits this, so its mid-stream reconfiguration silently fails. */
    const bool was_running = s_snk.running;
    if (was_running) {
        ESP_RETURN_ON_ERROR(audio_sink_stop(), TAG, "stop before reconfigure failed");
    }

    i2s_std_clk_config_t clk = clk_config_for(sample_rate_hz);
    i2s_std_slot_config_t slot = slot_config_for(channels);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_snk.tx, &clk), TAG,
                        "clock reconfigure failed");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_snk.tx, &slot), TAG,
                        "slot reconfigure failed");

    s_snk.sample_rate_hz = sample_rate_hz;
    s_snk.channels = channels;
    ESP_LOGI(TAG, "format now %" PRIu32 " Hz, %u ch", sample_rate_hz, channels);

    if (was_running) {
        ESP_RETURN_ON_ERROR(audio_sink_start(), TAG, "restart after reconfigure failed");
    }
    return ESP_OK;
}

void audio_sink_set_processor(audio_sink_process_cb_t cb)
{
    s_snk.processor = cb;
}

void audio_sink_set_volume(uint8_t avrcp_volume)
{
    if (avrcp_volume > 0x7f) {
        avrcp_volume = 0x7f;
    }

    int32_t gain;
    if (avrcp_volume == 0) {
        gain = 0;
    } else if (avrcp_volume == 0x7f) {
        gain = GAIN_UNITY;
    } else {
        /* Cubic taper, computed once per volume change rather than per sample.
         * The float cost here is irrelevant; in the writer loop it would not
         * be. */
        const float norm = (float)avrcp_volume / 127.0f;
        gain = (int32_t)(GAIN_UNITY * norm * norm * norm);
    }

    s_snk.gain_q15 = gain;
    ESP_LOGD(TAG, "volume %u -> gain %" PRId32 "/%d", avrcp_volume, gain, GAIN_UNITY);
}

size_t audio_sink_write(const uint8_t *data, size_t size)
{
    if (s_snk.ringbuf == NULL || data == NULL || size == 0) {
        return 0;
    }

    /* Refuse audio while the output is stopped. Accepting it filled the buffer
     * during a stop or a format change, and a full buffer plus a parked writer
     * is a state nothing recovers from. */
    if (!s_snk.running) {
        return 0;
    }

    size_t buffered = 0;
    vRingbufferGetInfo(s_snk.ringbuf, NULL, NULL, NULL, NULL, &buffered);

    if (s_snk.mode == RB_DROPPING) {
        /* Stay in drop mode until the writer has clawed back real headroom;
         * resuming at the first free byte just overflows again. */
        s_snk.stats.dropped++;
        if (buffered <= PREFETCH_BYTES) {
            s_snk.mode = RB_PLAYING;
            /* This edge had no wakeup either: leaving drop mode with a parked
             * writer left the buffer full forever. */
            xTaskNotifyGive(s_snk.writer);
        }
        return 0;
    }

    if (xRingbufferSend(s_snk.ringbuf, data, size, 0) != pdTRUE) {
        s_snk.mode = RB_DROPPING;
        s_snk.stats.dropped++;
        /* Throttled: this runs in Bluetooth stack context, the log is a
         * blocking UART write, and a source pushing faster than realtime makes
         * the buffer oscillate around the drop threshold several times a
         * second. The counter carries the real signal. */
        const int64_t now = esp_timer_get_time();
        if (now - s_snk.last_drop_log_us > 5000000) {
            s_snk.last_drop_log_us = now;
            ESP_LOGW(TAG, "ring buffer full, shedding audio (%" PRIu32 " dropped so far)",
                     s_snk.stats.dropped);
        }
        return 0;
    }
    s_snk.stats.packets++;

    if (s_snk.mode == RB_PREFETCHING && buffered + size >= PREFETCH_BYTES) {
        s_snk.mode = RB_PLAYING;
        if (s_snk.running) {
            xTaskNotifyGive(s_snk.writer);
        }
    }
    return size;
}

void audio_sink_get_stats(audio_sink_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_snk.stats;
    out->buffered = 0;
    if (s_snk.ringbuf != NULL) {
        vRingbufferGetInfo(s_snk.ringbuf, NULL, NULL, NULL, NULL, &out->buffered);
    }
}
