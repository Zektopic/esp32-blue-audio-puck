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

/**
 * The three front-panel buttons.
 *
 * Named by position rather than by function on purpose. What a press *means*
 * is the application's business -- this component only reports which button
 * moved and how long it was held.
 */
typedef enum {
    PUCK_UI_BUTTON_1 = 0,
    PUCK_UI_BUTTON_2,
    PUCK_UI_BUTTON_3,
    PUCK_UI_BUTTON_COUNT,
} puck_ui_button_t;

/** What a button did. */
typedef enum {
    PUCK_UI_TAP = 0,      /*!< released before the hold threshold */
    PUCK_UI_HOLD,         /*!< crossed the hold threshold, still down */
    PUCK_UI_HOLD_REPEAT,  /*!< still down; repeats for continuous actions */
    PUCK_UI_HOLD_EXTRA,   /*!< crossed a much longer threshold */
} puck_ui_event_t;

/** Called on the UI task. */
typedef void (*puck_ui_button_cb_t)(puck_ui_button_t button, puck_ui_event_t event);

/**
 * @brief Configure the buttons and status LED and start the UI task.
 *
 * Buttons are inputs with pull-ups, active low: wire each one between its pin
 * and ground, no external resistor. A button configured to GPIO -1 is skipped,
 * so a two-button build costs nothing.
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
 * @brief Register the button handler.
 */
void puck_ui_set_button_cb(puck_ui_button_cb_t cb);

#ifdef __cplusplus
}
#endif
