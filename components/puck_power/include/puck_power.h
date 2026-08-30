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

/** How busy the puck is, which decides how aggressively it may idle down. */
typedef enum {
    PUCK_POWER_IDLE = 0,   /*!< no source connected; the shutdown clock runs */
    PUCK_POWER_LINKED,     /*!< connected but not streaming */
    PUCK_POWER_STREAMING,  /*!< audio flowing; never sleep */
} puck_power_activity_t;

/**
 * @brief Apply the radio and clock policy, and arm the idle shutdown timer.
 *
 * Call after the Bluetooth controller is enabled: transmit power and
 * controller sleep can only be set on a running controller.
 */
esp_err_t puck_power_init(void);

/**
 * @brief Tell the power manager what the link is doing.
 *
 * Restarts or cancels the idle shutdown timer as appropriate. Safe to call
 * repeatedly with the same value.
 */
void puck_power_set_activity(puck_power_activity_t activity);

/**
 * @brief Push the idle shutdown out without changing activity state.
 *
 * Call from user interaction, so pressing a button on a puck nobody has
 * connected to yet keeps it awake a while longer.
 */
void puck_power_kick(void);

/**
 * @brief Shut the radio down and enter deep sleep now.
 *
 * Does not return. The main button wakes the chip, which restarts from
 * app_main -- deep sleep does not preserve RAM.
 */
void puck_power_sleep_now(void);

/**
 * @brief Whether this boot was a wake from deep sleep rather than a cold start.
 */
bool puck_power_woke_from_sleep(void);

#ifdef __cplusplus
}
#endif
