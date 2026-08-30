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

/**
 * @brief Handler for work dispatched onto the application task.
 *
 * @param event  Event id supplied to bt_core_dispatch().
 * @param param  Heap copy of the caller's parameters, freed after this returns.
 */
typedef void (*bt_core_work_t)(uint16_t event, void *param);

/**
 * @brief Optional deep-copy hook, called after the shallow memcpy.
 */
typedef void (*bt_core_copy_t)(void *dst, void *src, int len);

/**
 * @brief Releases whatever the matching deep-copy hook allocated.
 *
 * Runs on every disposal path -- after the handler returns, when the queue is
 * full, and when the task is stopped -- so a dropped message cannot leak.
 * Pair it with @ref bt_core_copy_t; supplying one without the other leaks.
 */
typedef void (*bt_core_free_t)(void *param);

/**
 * @brief Bring up NVS, the BR/EDR controller and Bluedroid.
 *
 * Releases the BLE half of the controller's memory first: this firmware is
 * Classic-only, and that memory is worth roughly 30 kB of heap.
 */
esp_err_t bt_core_stack_init(void);

/**
 * @brief Start the application task that runs dispatched work.
 */
esp_err_t bt_core_task_start(void);

/**
 * @brief Stop the application task and release its queue.
 */
void bt_core_task_stop(void);

/**
 * @brief Move work out of a Bluetooth stack callback and onto the app task.
 *
 * Stack callbacks run in Bluedroid's own context, where blocking or slow work
 * stalls the link. Anything non-trivial belongs here instead.
 *
 * Never blocks: a full queue drops the work and returns false rather than
 * stalling the stack. Callers that own durable state must not assume delivery.
 *
 * @param work       Handler to run on the application task.
 * @param event      Event id passed through to @p work.
 * @param params     Parameters to copy, or NULL.
 * @param param_len  Size of @p params in bytes, or 0.
 * @param copy       Deep-copy hook for pointer members, or NULL.
 * @param dtor       Frees what @p copy allocated, or NULL. Required whenever
 *                   @p copy is supplied.
 *
 * @return true if the work was queued.
 */
bool bt_core_dispatch(bt_core_work_t work, uint16_t event, void *params, int param_len,
                      bt_core_copy_t copy, bt_core_free_t dtor);

/**
 * @brief Number of remembered (bonded) sources.
 */
int bt_core_bond_count(void);

/**
 * @brief Forget every bonded source.
 *
 * The puck has no way to show which devices it remembers, so the only usable
 * bond management is "forget them all and start again".
 */
esp_err_t bt_core_forget_bonds(void);

/**
 * @brief Format a Bluetooth device address as "aa:bb:cc:dd:ee:ff".
 *
 * @param bda   Six-byte address.
 * @param out   Destination buffer.
 * @param size  Size of @p out; must be at least 18.
 *
 * @return @p out, or NULL if the arguments are unusable.
 */
const char *bt_core_bda_str(const uint8_t *bda, char *out, size_t size);

#ifdef __cplusplus
}
#endif
