/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** AVRCP absolute volume is a 7-bit scale. */
#define PUCK_VOLUME_MAX 0x7f

/** Track metadata as reported by the source. Fields may be empty. */
typedef struct {
    char title[64];
    char artist[64];
    char album[64];
} puck_track_info_t;

/** Called on the application task when metadata or playback state changes. */
typedef void (*puck_avrcp_track_cb_t)(const puck_track_info_t *info);

/**
 * @brief Start AVRCP in both roles.
 *
 * Controller (CT) so the puck can ask the phone for track metadata and drive
 * transport keys; target (TG) so the phone can set absolute volume and the
 * puck can report volume changes back. Call after the A2DP sink is up.
 */
esp_err_t puck_avrcp_init(void);

/**
 * @brief Current volume, 0..PUCK_VOLUME_MAX.
 */
uint8_t puck_avrcp_get_volume(void);

/**
 * @brief Change volume locally, e.g. from a button.
 *
 * Applies the new gain and, if the source subscribed to volume notifications,
 * tells it so its on-screen slider follows.
 */
void puck_avrcp_set_volume(uint8_t volume);

/**
 * @brief Adjust volume by @p delta steps, clamped to the valid range.
 */
void puck_avrcp_adjust_volume(int16_t delta);

/**
 * @brief Record which device holds the audio stream, or NULL to clear it.
 *
 * Absolute volume commands carry no address of their own, so this is what
 * lets the puck refuse volume changes from a device that bonded once and is
 * not the one currently playing.
 */
void puck_avrcp_set_audio_peer(const uint8_t *bda);

/**
 * @brief Send a transport key to the source, e.g. ESP_AVRC_PT_CMD_PLAY.
 *
 * Sends press and release, which is what sources expect; a press alone is
 * treated as a held key and repeats.
 */
esp_err_t puck_avrcp_send_key(uint8_t key_code);

/**
 * @brief Register a callback for track metadata updates.
 */
void puck_avrcp_set_track_cb(puck_avrcp_track_cb_t cb);

/**
 * @brief Most recently reported track metadata.
 */
void puck_avrcp_get_track(puck_track_info_t *out);

/**
 * @brief Whether a source is currently playing.
 */
bool puck_avrcp_is_playing(void);

#ifdef __cplusplus
}
#endif
