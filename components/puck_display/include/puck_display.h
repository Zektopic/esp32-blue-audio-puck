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

/** What the screen should be showing. */
typedef enum {
    PUCK_SCREEN_SPLASH = 0,  /*!< name and address while the stack comes up */
    PUCK_SCREEN_PAIRING,     /*!< discoverable, waiting for a source */
    PUCK_SCREEN_IDLE,        /*!< connectable but nothing connected */
    PUCK_SCREEN_NOW_PLAYING, /*!< track, state, volume, battery */
} puck_screen_t;

/**
 * @brief Bring up the I2C bus and the panel, and start the refresh task.
 *
 * With no panel fitted, or if the panel does not answer on the bus, this logs
 * and returns ESP_OK with the display disabled. A missing screen must not stop
 * a music player from playing music.
 */
esp_err_t puck_display_init(void);

/**
 * @brief Whether a panel actually answered on the bus.
 */
bool puck_display_present(void);

/**
 * @brief Choose the screen to show. Cheap; safe to call from event handlers.
 */
void puck_display_set_screen(puck_screen_t screen);

/**
 * @brief Show a transient message over the current screen, e.g. "Volume 60%".
 *
 * @param text  Copied; may be freed by the caller immediately.
 * @param ms    How long to hold it before returning to the normal screen.
 */
void puck_display_toast(const char *text, uint32_t ms);

/**
 * @brief Blank the panel and stop refreshing it.
 *
 * Called before deep sleep: an OLED left lit would undo the power work.
 */
void puck_display_off(void);

/**
 * @brief Draw the full-screen test pattern and hold it briefly.
 *
 * Needs no font, so it separates "the bus and the panel work" from "the text
 * rendering is right" -- two faults with completely different fixes.
 */
void puck_display_self_test(void);

#ifdef __cplusplus
}
#endif
