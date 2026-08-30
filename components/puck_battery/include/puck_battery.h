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

/** Where the puck is getting its power from. */
typedef enum {
    PUCK_BATTERY_ABSENT = 0,  /*!< no sense divider fitted; assume USB power */
    PUCK_BATTERY_OK,
    PUCK_BATTERY_LOW,         /*!< below the warning threshold */
    PUCK_BATTERY_CRITICAL,    /*!< low enough that a shutdown is warranted */
} puck_battery_state_t;

/** A reading. @c valid is false until the first sample has been taken. */
typedef struct {
    bool                 valid;
    uint16_t             millivolts;  /*!< at the cell, after the divider maths */
    uint8_t              percent;     /*!< 0..100, from the discharge curve */
    puck_battery_state_t state;
} puck_battery_reading_t;

/** Called on the sampling task when the state changes, e.g. OK -> LOW. */
typedef void (*puck_battery_cb_t)(const puck_battery_reading_t *reading);

/**
 * @brief Start sampling the battery.
 *
 * With no sense pin configured this succeeds and reports PUCK_BATTERY_ABSENT
 * forever, so callers need no special case for a USB-powered build.
 */
esp_err_t puck_battery_init(void);

/**
 * @brief Latest reading. Cheap; safe to call from a display refresh.
 */
void puck_battery_get(puck_battery_reading_t *out);

/**
 * @brief Register a handler for state transitions.
 */
void puck_battery_set_cb(puck_battery_cb_t cb);

/**
 * @brief Force a sample now rather than waiting for the next interval.
 */
void puck_battery_sample_now(void);

#ifdef __cplusplus
}
#endif
