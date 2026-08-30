/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Runtime counters, for logging and for judging buffer sizing on real links. */
typedef struct {
    uint32_t packets;       /*!< A2DP payloads accepted into the ring buffer */
    uint32_t dropped;       /*!< payloads discarded because the buffer was full */
    uint32_t underruns;     /*!< times the writer ran dry and re-prefetched */
    size_t   buffered;      /*!< bytes currently waiting in the ring buffer */
} audio_sink_stats_t;

/**
 * @brief Create the I2S channel, ring buffer and writer task.
 *
 * Call once at start-up. The writer task is created here and parked, rather
 * than spun up per connection, so that a disconnect can never delete a task
 * that is mid-write.
 */
esp_err_t audio_sink_init(void);

/**
 * @brief Release everything audio_sink_init() created.
 */
void audio_sink_deinit(void);

/**
 * @brief Enable the I2S channel and let the writer task run.
 */
esp_err_t audio_sink_start(void);

/**
 * @brief Disable the I2S channel and park the writer task.
 */
esp_err_t audio_sink_stop(void);

/**
 * @brief Set the PCM format arriving from the decoder.
 *
 * The I2S driver only accepts a reconfiguration while the channel is disabled,
 * so a running stream is briefly stopped and restarted around the change.
 *
 * @param sample_rate_hz  44100, 48000, 32000 or 16000.
 * @param channels        1 for mono, 2 for stereo.
 */
esp_err_t audio_sink_set_format(uint32_t sample_rate_hz, uint8_t channels);

/**
 * @brief Set playback gain from an AVRCP absolute volume value.
 *
 * The mapping is cubic rather than linear: volume sliders are perceptual, and
 * a linear gain makes the bottom two thirds of the travel do almost nothing.
 * At full scale the gain is exactly unity and the scaling pass is skipped.
 *
 * @param avrcp_volume  0..127, as carried by AVRCP.
 */
void audio_sink_set_volume(uint8_t avrcp_volume);

/**
 * @brief Hand decoded PCM to the writer task. Safe to call from stack context.
 *
 * Never blocks: if the ring buffer is full the payload is dropped and counted.
 *
 * @return Bytes accepted, or 0 if the payload was dropped.
 */
size_t audio_sink_write(const uint8_t *data, size_t size);

/**
 * @brief Read the running counters.
 */
void audio_sink_get_stats(audio_sink_stats_t *out);

#ifdef __cplusplus
}
#endif
