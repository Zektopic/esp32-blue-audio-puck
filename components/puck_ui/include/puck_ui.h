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

/** What the status LED is telling the user. */
typedef enum {
    PUCK_UI_BOOTING = 0,  /*!< solid dim while the stack comes up */
    PUCK_UI_PAIRING,      /*!< fast blink: discoverable, waiting for a source */
    PUCK_UI_CONNECTED,    /*!< slow breathe: linked but not streaming */
    PUCK_UI_PLAYING,      /*!< steady: audio is flowing */
    PUCK_UI_FAULT,        /*!< triple flash: something failed to start */
} puck_ui_state_t;

/** Gestures the main button can produce. */
typedef enum {
    PUCK_UI_PRESS_SINGLE = 0,
    PUCK_UI_PRESS_DOUBLE,
    PUCK_UI_PRESS_TRIPLE,
    PUCK_UI_PRESS_LONG,     /*!< held past the long-press threshold */
    PUCK_UI_PRESS_VERY_LONG,/*!< held much longer: destructive actions */
    PUCK_UI_VOLUME_UP,      /*!< optional volume button, repeats while held */
    PUCK_UI_VOLUME_DOWN,
} puck_ui_gesture_t;

/** Called on the UI task when a gesture completes. */
typedef void (*puck_ui_gesture_cb_t)(puck_ui_gesture_t gesture);

/**
 * @brief Configure the button(s) and status LED and start the UI task.
 *
 * Buttons are inputs with pull-ups, active low. Buttons configured to GPIO -1
 * are skipped, so a build with no volume buttons costs nothing.
 */
esp_err_t puck_ui_init(void);

/**
 * @brief Set what the LED should be indicating.
 */
void puck_ui_set_state(puck_ui_state_t state);

/**
 * @brief Current LED state.
 */
puck_ui_state_t puck_ui_get_state(void);

/**
 * @brief Register the gesture handler.
 */
void puck_ui_set_gesture_cb(puck_ui_gesture_cb_t cb);

#ifdef __cplusplus
}
#endif
