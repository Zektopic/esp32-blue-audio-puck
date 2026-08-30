/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * Parametric equaliser: a cascade of biquad sections per channel.
 *
 * Single-precision float rather than fixed point. The ESP32 has a hardware
 * FPU, and at 44.1 kHz stereo with five bands the whole cascade costs a few
 * hundred thousand multiply-adds per second -- far below what the FPU does in
 * the time available, and worth the readability and the freedom from
 * coefficient scaling headaches.
 *
 * Coefficients follow the Audio EQ Cookbook (Robert Bristow-Johnson).
 */

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_dsp.h"

static const char *TAG = "audio_dsp";

#define MAX_CHANNELS 2

/** Normalised biquad coefficients, a0 divided out. */
typedef struct {
    float b0, b1, b2, a1, a2;
    bool  bypass;   /*!< true when the band is flat; skips the section */
} biquad_coeffs_t;

/** Transposed direct form II state -- two words per section per channel. */
typedef struct {
    float s1, s2;
} biquad_state_t;

typedef struct {
    uint32_t         sample_rate_hz;
    volatile bool    enabled;
    audio_dsp_band_t bands[AUDIO_DSP_BANDS];

    /*
     * Coefficients are double buffered and published by flipping an index.
     *
     * A biquad_coeffs_t is five floats plus a flag. No amount of `volatile`
     * makes a 24-byte struct update atomic, so a writer reading it per sample
     * while the application task rewrites it in place can observe a half
     * published section. That is not a glitch that passes: a torn (a1, a2)
     * pair can put the poles outside the unit circle, and a recursive filter
     * with unstable poles grows until saturation turns it into a full-scale
     * square wave that persists long after the coefficients are consistent.
     *
     * The application task fills the inactive set, then flips. The writer
     * reads the index once per block. Cost in the inner loop: nothing.
     */
    biquad_coeffs_t  sets[2][AUDIO_DSP_BANDS];
    /* How many entries of sets[i] are live. Flat bands are left out of the
     * published set entirely rather than marked and skipped: a flat equaliser
     * is the common case, and scanning five bypass flags per sample to
     * discover there is nothing to do is pure waste. Zero means the whole
     * pass returns immediately. */
    uint8_t          set_count[2];
    volatile uint8_t active;          /*!< which set the writer reads */
    volatile bool    reset_pending;   /*!< writer clears its own state */

    /* Owned exclusively by the audio writer task. */
    biquad_state_t   state[MAX_CHANNELS][AUDIO_DSP_BANDS];
} audio_dsp_t;

static audio_dsp_t s_dsp;

/**
 * Compute one section's coefficients.
 *
 * A band at 0 dB is marked bypass rather than given unity coefficients: unity
 * still costs five multiplies per sample, and a flat equaliser is the common
 * case.
 */
static void compute_coeffs(const audio_dsp_band_t *band, uint32_t sample_rate_hz,
                           biquad_coeffs_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Guard the degenerate cases before they reach tan()/sin(): a corner at or
     * above Nyquist has no meaning, and a zero Q divides by zero. */
    if (fabsf(band->gain_db) < 0.01f || band->freq_hz <= 0.0f || band->q <= 0.0f ||
        band->freq_hz >= (float)sample_rate_hz * 0.5f) {
        out->bypass = true;
        return;
    }

    const float A     = powf(10.0f, band->gain_db / 40.0f);
    const float w0    = 2.0f * (float)M_PI * band->freq_hz / (float)sample_rate_hz;
    const float cosw0 = cosf(w0);
    const float sinw0 = sinf(w0);
    const float alpha = sinw0 / (2.0f * band->q);

    float b0, b1, b2, a0, a1, a2;

    switch (band->type) {
    case AUDIO_DSP_LOW_SHELF: {
        const float sqrtA = sqrtf(A);
        const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
        b0 =        A * ((A + 1.0f) - (A - 1.0f) * cosw0 + twoSqrtAalpha);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
        b2 =        A * ((A + 1.0f) - (A - 1.0f) * cosw0 - twoSqrtAalpha);
        a0 =             (A + 1.0f) + (A - 1.0f) * cosw0 + twoSqrtAalpha;
        a1 =    -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
        a2 =             (A + 1.0f) + (A - 1.0f) * cosw0 - twoSqrtAalpha;
        break;
    }
    case AUDIO_DSP_HIGH_SHELF: {
        const float sqrtA = sqrtf(A);
        const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
        b0 =        A * ((A + 1.0f) + (A - 1.0f) * cosw0 + twoSqrtAalpha);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
        b2 =        A * ((A + 1.0f) + (A - 1.0f) * cosw0 - twoSqrtAalpha);
        a0 =             (A + 1.0f) - (A - 1.0f) * cosw0 + twoSqrtAalpha;
        a1 =     2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
        a2 =             (A + 1.0f) - (A - 1.0f) * cosw0 - twoSqrtAalpha;
        break;
    }
    case AUDIO_DSP_PEAKING:
    default:
        b0 =  1.0f + alpha * A;
        b1 = -2.0f * cosw0;
        b2 =  1.0f - alpha * A;
        a0 =  1.0f + alpha / A;
        a1 = -2.0f * cosw0;
        a2 =  1.0f - alpha / A;
        break;
    }

    const float inv_a0 = 1.0f / a0;
    out->b0 = b0 * inv_a0;
    out->b1 = b1 * inv_a0;
    out->b2 = b2 * inv_a0;
    out->a1 = a1 * inv_a0;
    out->a2 = a2 * inv_a0;
    out->bypass = false;
}

/**
 * Recompute every section into the inactive set and publish it atomically.
 *
 * Always asks the writer to clear its filter state: compaction means a section
 * can change slots, and state belonging to a different filter rings.
 */
static void publish_coeffs(void)
{
    const uint8_t next = s_dsp.active ^ 1u;
    uint8_t count = 0;

    for (size_t i = 0; i < AUDIO_DSP_BANDS; i++) {
        biquad_coeffs_t candidate;
        compute_coeffs(&s_dsp.bands[i], s_dsp.sample_rate_hz, &candidate);
        if (!candidate.bypass) {
            s_dsp.sets[next][count++] = candidate;
        }
    }
    s_dsp.set_count[next] = count;

    /* Everything above must land before the index flip below, or the writer
     * can follow the new index to a set that is still being written. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_dsp.active = next;
    s_dsp.reset_pending = true;
}

esp_err_t audio_dsp_init(uint32_t sample_rate_hz)
{
    ESP_RETURN_ON_FALSE(sample_rate_hz > 0, ESP_ERR_INVALID_ARG, TAG, "bad sample rate");

    memset(&s_dsp, 0, sizeof(s_dsp));
    s_dsp.sample_rate_hz = sample_rate_hz;

    /* A sensible flat starting layout: shelves at the ends, bells across the
     * middle spaced roughly by octaves. All at 0 dB, so all bypassed. */
    static const float default_freqs[] = {80.0f, 300.0f, 1000.0f, 3500.0f, 10000.0f};
    for (size_t i = 0; i < AUDIO_DSP_BANDS; i++) {
        s_dsp.bands[i].freq_hz = (i < sizeof(default_freqs) / sizeof(default_freqs[0]))
                                     ? default_freqs[i]
                                     : 1000.0f;
        s_dsp.bands[i].q = 0.9f;
        s_dsp.bands[i].gain_db = 0.0f;
        s_dsp.bands[i].type = AUDIO_DSP_PEAKING;
    }
    if (AUDIO_DSP_BANDS >= 2) {
        s_dsp.bands[0].type = AUDIO_DSP_LOW_SHELF;
        s_dsp.bands[AUDIO_DSP_BANDS - 1].type = AUDIO_DSP_HIGH_SHELF;
    }

    /* Populate both sets so the very first block reads a complete one. */
    publish_coeffs();
    publish_coeffs();
    memset(s_dsp.state, 0, sizeof(s_dsp.state));
#ifdef CONFIG_PUCK_EQ_ENABLED_AT_BOOT
    s_dsp.enabled = true;
#else
    s_dsp.enabled = false;
#endif

    ESP_LOGI(TAG, "%d-band equaliser ready at %" PRIu32 " Hz (%s, %u active)",
             AUDIO_DSP_BANDS, sample_rate_hz, s_dsp.enabled ? "on" : "off",
             s_dsp.set_count[s_dsp.active]);
    return ESP_OK;
}

esp_err_t audio_dsp_set_sample_rate(uint32_t sample_rate_hz)
{
    ESP_RETURN_ON_FALSE(sample_rate_hz > 0, ESP_ERR_INVALID_ARG, TAG, "bad sample rate");

    if (sample_rate_hz == s_dsp.sample_rate_hz) {
        return ESP_OK;
    }
    s_dsp.sample_rate_hz = sample_rate_hz;
    /* Old state belongs to the old rate; carrying it over would ring. */
    publish_coeffs();
    ESP_LOGI(TAG, "coefficients recomputed for %" PRIu32 " Hz", sample_rate_hz);
    return ESP_OK;
}

esp_err_t audio_dsp_set_band(size_t index, const audio_dsp_band_t *band)
{
    ESP_RETURN_ON_FALSE(index < AUDIO_DSP_BANDS, ESP_ERR_INVALID_ARG, TAG,
                        "band %u out of range", (unsigned)index);
    ESP_RETURN_ON_FALSE(band != NULL, ESP_ERR_INVALID_ARG, TAG, "null band");

    s_dsp.bands[index] = *band;
    /* Republish the whole set rather than editing one section in place: the
     * writer must never see a set that is part old and part new. */
    publish_coeffs();
    ESP_LOGI(TAG, "band %u: %.0f Hz, Q %.2f, %+.1f dB (%u section(s) active)",
             (unsigned)index, band->freq_hz, band->q, band->gain_db,
             s_dsp.set_count[s_dsp.active]);
    return ESP_OK;
}

esp_err_t audio_dsp_get_band(size_t index, audio_dsp_band_t *out)
{
    ESP_RETURN_ON_FALSE(index < AUDIO_DSP_BANDS && out != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bad arguments");
    *out = s_dsp.bands[index];
    return ESP_OK;
}

void audio_dsp_set_enabled(bool enabled)
{
    if (enabled && !s_dsp.enabled) {
        /* Starting from stale state would produce a transient. Ask the writer
         * to do it -- the state is its alone. */
        s_dsp.reset_pending = true;
    }
    s_dsp.enabled = enabled;
    ESP_LOGI(TAG, "equaliser %s", enabled ? "on" : "off");
}

bool audio_dsp_is_enabled(void)
{
    return s_dsp.enabled;
}

void audio_dsp_reset(void)
{
    for (size_t i = 0; i < AUDIO_DSP_BANDS; i++) {
        s_dsp.bands[i].gain_db = 0.0f;
    }
    publish_coeffs();
    ESP_LOGI(TAG, "equaliser reset to flat");
}

/** Round and clamp back into int16, so a boosted band clips instead of wrapping. */
static inline int16_t saturate(float sample)
{
    if (sample > 32767.0f) {
        return 32767;
    }
    if (sample < -32768.0f) {
        return -32768;
    }
    /* Round rather than truncate. A cast toward zero biases every sample by up
     * to half an LSB, which across a whole stream is a small DC-ish distortion
     * for no reason: lrintf is a single instruction here. */
    return (int16_t)lrintf(sample);
}

uint32_t audio_dsp_benchmark(size_t block_samples, unsigned iterations)
{
    if (block_samples == 0 || iterations == 0) {
        return 0;
    }

    int16_t *block = calloc(block_samples, sizeof(int16_t));
    if (block == NULL) {
        ESP_LOGE(TAG, "benchmark buffer alloc failed");
        return 0;
    }

    /* Full-scale alternating samples: the filters see real signal energy, and
     * denormals never appear to flatter the result. */
    for (size_t i = 0; i < block_samples; i++) {
        block[i] = (i & 1) ? 24000 : -24000;
    }

    /* Measure the worst case: every band doing work, whether or not the
     * equaliser is switched on and whatever the user has it set to. A flat
     * equaliser costs nothing by design, so timing that would answer nothing.
     * The real configuration is restored before returning. */
    const bool was_enabled = s_dsp.enabled;
    audio_dsp_band_t saved[AUDIO_DSP_BANDS];
    memcpy(saved, s_dsp.bands, sizeof(saved));

    for (size_t i = 0; i < AUDIO_DSP_BANDS; i++) {
        s_dsp.bands[i].gain_db = 3.0f;
    }
    publish_coeffs();
    s_dsp.enabled = true;

    const int64_t started = esp_timer_get_time();
    for (unsigned i = 0; i < iterations; i++) {
        audio_dsp_process(block, block_samples, 2);
    }
    const int64_t elapsed = esp_timer_get_time() - started;

    const uint8_t measured_sections = s_dsp.set_count[s_dsp.active];

    s_dsp.enabled = was_enabled;
    memcpy(s_dsp.bands, saved, sizeof(saved));
    publish_coeffs();
    free(block);

    const uint32_t per_block_us = (uint32_t)(elapsed / iterations);
    const uint32_t frames = (uint32_t)(block_samples / 2);
    const uint32_t realtime_us = (frames * 1000000u) / (s_dsp.sample_rate_hz ?
                                                        s_dsp.sample_rate_hz : 44100u);

    ESP_LOGI(TAG, "benchmark: %u active section(s), %u frames in %" PRIu32 " us "
                  "(%" PRIu32 " us of audio, %" PRIu32 "%% of one core)",
             measured_sections, (unsigned)frames, per_block_us,
             realtime_us, realtime_us ? (per_block_us * 100u) / realtime_us : 0u);

    return per_block_us;
}

void audio_dsp_process(int16_t *samples, size_t count, uint8_t channels)
{
    if (!s_dsp.enabled || samples == NULL || count == 0) {
        return;
    }
    if (channels < 1 || channels > MAX_CHANNELS) {
        return;
    }

    /* Read the published set once per block. Re-reading it per sample is what
     * would let a coefficient update tear across the middle of a waveform. */
    const uint8_t set_index = s_dsp.active;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint8_t sections = s_dsp.set_count[set_index];

    /* Every band flat: nothing to do at all, which is the default state. */
    if (sections == 0) {
        return;
    }

    if (s_dsp.reset_pending) {
        s_dsp.reset_pending = false;
        memset(s_dsp.state, 0, sizeof(s_dsp.state));
    }

    /* Copy the live sections onto the stack before the sample loop. Read
     * through the global they would be reloaded through a pointer on every
     * sample of every band; as locals the compiler keeps them in registers. */
    biquad_coeffs_t coeffs[AUDIO_DSP_BANDS];
    memcpy(coeffs, s_dsp.sets[set_index], (size_t)sections * sizeof(coeffs[0]));

    /* Walk whole frames with a running index. Deriving the channel with
     * `i % channels` would put an integer division in the innermost audio
     * loop, which is the one place it cannot be afforded. */
    size_t i = 0;
    while (i + channels <= count) {
        for (uint8_t ch = 0; ch < channels; ch++, i++) {
            float x = (float)samples[i];

            for (size_t b = 0; b < sections; b++) {
                const biquad_coeffs_t *c = &coeffs[b];
                biquad_state_t *st = &s_dsp.state[ch][b];

                /* Transposed direct form II: two state words, and better
                 * numerical behaviour than DF-I at these coefficient ranges. */
                const float y = c->b0 * x + st->s1;
                st->s1 = c->b1 * x - c->a1 * y + st->s2;
                st->s2 = c->b2 * x - c->a2 * y;
                x = y;
            }

            samples[i] = saturate(x);
        }
    }
}
