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

/** Number of biquad sections per channel. */
#define AUDIO_DSP_BANDS CONFIG_PUCK_EQ_BANDS

/** Filter shape for one band. */
typedef enum {
    AUDIO_DSP_PEAKING = 0,  /*!< bell centred on freq_hz */
    AUDIO_DSP_LOW_SHELF,    /*!< everything below freq_hz */
    AUDIO_DSP_HIGH_SHELF,   /*!< everything above freq_hz */
} audio_dsp_band_type_t;

/** One parametric band. A gain of 0 dB is a no-op regardless of the rest. */
typedef struct {
    audio_dsp_band_type_t type;
    float                 freq_hz;
    float                 q;        /*!< 0.7 is a broad bell, 4 is narrow */
    float                 gain_db;  /*!< cut or boost, roughly -24..+24 */
} audio_dsp_band_t;

/**
 * @brief Prepare the equaliser with a default flat curve.
 */
esp_err_t audio_dsp_init(uint32_t sample_rate_hz);

/**
 * @brief Recompute every coefficient for a new sample rate.
 *
 * Biquad coefficients are a function of freq/sample_rate, so a source that
 * negotiates 48 kHz instead of 44.1 kHz would otherwise shift the whole curve
 * up by about 9%. Call this whenever the stream format changes.
 */
esp_err_t audio_dsp_set_sample_rate(uint32_t sample_rate_hz);

/**
 * @brief Configure one band and recompute its coefficients.
 *
 * @param index  0 .. AUDIO_DSP_BANDS-1
 */
esp_err_t audio_dsp_set_band(size_t index, const audio_dsp_band_t *band);

/**
 * @brief Read back one band.
 */
esp_err_t audio_dsp_get_band(size_t index, audio_dsp_band_t *out);

/**
 * @brief Turn the whole equaliser on or off.
 *
 * When off, audio_dsp_process() returns immediately, so a flat setup costs
 * nothing per sample.
 */
void audio_dsp_set_enabled(bool enabled);

/** @brief Whether the equaliser is currently applied. */
bool audio_dsp_is_enabled(void);

/**
 * @brief Reset every band to 0 dB and clear the filter state.
 */
void audio_dsp_reset(void);

/**
 * @brief Filter a block of interleaved 16-bit samples in place.
 *
 * Runs on the audio writer task. Safe to call with the equaliser disabled.
 *
 * @param samples   Interleaved PCM, modified in place.
 * @param count     Number of int16 samples, not frames.
 * @param channels  1 or 2; stereo keeps independent filter state per channel.
 */
void audio_dsp_process(int16_t *samples, size_t count, uint8_t channels);

#ifdef __cplusplus
}
#endif
