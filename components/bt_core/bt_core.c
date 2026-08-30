/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * Bluetooth plumbing shared by the rest of the firmware: stack bring-up, and a
 * single-threaded work queue that stack callbacks hand slow work to.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_bt_defs.h"

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "bt_core.h"

static const char *TAG = "bt_core";

#define BT_CORE_QUEUE_LEN     10
#define BT_CORE_TASK_STACK    3072
#define BT_CORE_TASK_PRIO     10

typedef struct {
    bt_core_work_t work;
    bt_core_free_t dtor;    /*!< frees nested allocations, on every path */
    uint16_t       event;
    void          *param;
} bt_core_msg_t;

static QueueHandle_t   s_queue;
static TaskHandle_t    s_task;

/* Link quality polling. */
static esp_timer_handle_t    s_rssi_timer;
static esp_bd_addr_t         s_rssi_peer;
static bool                  s_rssi_peer_valid;
static bt_core_link_quality_t s_link;
static volatile bool   s_task_should_exit;
static int64_t         s_last_drop_log_us;

/*
 * Release a message on any disposal path.
 *
 * The destructor has to run here rather than at the end of the handler: the
 * queue-full and shutdown paths never reach a handler, and those are exactly
 * the paths a hostile source can drive.
 */
static void bt_core_msg_release(bt_core_msg_t *msg)
{
    if (msg->dtor && msg->param) {
        msg->dtor(msg->param);
    }
    free(msg->param);
    msg->param = NULL;
}

static void bt_core_task(void *arg)
{
    bt_core_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.work) {
            msg.work(msg.event, msg.param);
        }
        bt_core_msg_release(&msg);

        if (s_task_should_exit) {
            break;
        }
    }

    /* Drain anything still queued so its nested allocations are freed, then
     * delete ourselves. Being deleted from outside could strand a mutex that a
     * handler was holding. */
    bt_core_msg_t leftover;
    while (xQueueReceive(s_queue, &leftover, 0) == pdTRUE) {
        bt_core_msg_release(&leftover);
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t bt_core_task_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(BT_CORE_QUEUE_LEN, sizeof(bt_core_msg_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Pin to the protocol CPU alongside the Bluetooth stack, so that the audio
     * writer task on the other core is never delayed by event handling. */
    if (xTaskCreatePinnedToCore(bt_core_task, "bt_app", BT_CORE_TASK_STACK, NULL,
                                BT_CORE_TASK_PRIO, &s_task, 0) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void bt_core_task_stop(void)
{
    if (s_task == NULL) {
        return;
    }

    /* Ask the task to leave rather than deleting it from outside. A handler
     * killed mid-run leaks its message and, worse, strands any mutex it holds
     * -- which would deadlock every later reader of that state. */
    s_task_should_exit = true;
    bt_core_dispatch(NULL, 0, NULL, 0, NULL, NULL);

    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "app task did not exit; leaving it running");
        s_task_should_exit = false;
        return;
    }

    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_task_should_exit = false;
}

bool bt_core_dispatch(bt_core_work_t work, uint16_t event, void *params, int param_len,
                      bt_core_copy_t copy, bt_core_free_t dtor)
{
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "dispatch before task start");
        return false;
    }

    bt_core_msg_t msg = {
        .work  = work,
        .dtor  = dtor,
        .event = event,
        .param = NULL,
    };

    if (param_len > 0) {
        if (params == NULL) {
            return false;
        }
        if ((msg.param = malloc(param_len)) == NULL) {
            ESP_LOGE(TAG, "out of memory queueing event %u", event);
            return false;
        }
        memcpy(msg.param, params, param_len);
        if (copy) {
            copy(msg.param, params, param_len);
        }
    }

    /* Zero timeout, deliberately. This runs in Bluedroid callback context: a
     * blocking send would let a source that floods events stall the stack for
     * 10 ms a time, which costs media packets and eventually the link. Dropping
     * is the designed outcome. */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        /* Throttled: the log is a blocking UART write, and it would otherwise
         * fire hardest exactly when the system is already saturated. */
        const int64_t now = esp_timer_get_time();
        if (now - s_last_drop_log_us > 1000000) {
            s_last_drop_log_us = now;
            ESP_LOGW(TAG, "work queue full, dropping event %u", event);
        }
        bt_core_msg_release(&msg);
        return false;
    }
    return true;
}

/**
 * Turn an RSSI delta into a four-bar meter.
 *
 * BR/EDR does not report absolute signal strength. It reports how far the
 * receiver sits from the Golden Receive Power Range -- the window the
 * controller is trying to keep the link inside -- so **zero means healthy**,
 * not absent. A link in good shape reads 0 most of the time, and the meter
 * sits full; the numbers only go negative once the link is genuinely
 * struggling, which is exactly when a signal indicator earns its place.
 *
 * The thresholds are judgement, not a specification. Treat the meter as
 * "fine / getting worse / about to drop out", not as dBm.
 */
static uint8_t bars_from_delta(int8_t delta)
{
    if (delta >= 0) {
        return 4;   /* inside or above the golden range */
    }
    if (delta >= -5) {
        return 3;
    }
    if (delta >= -15) {
        return 2;
    }
    if (delta >= -30) {
        return 1;
    }
    return 0;
}

static void rssi_poll_cb(void *arg)
{
    (void)arg;
    if (!s_rssi_peer_valid) {
        return;
    }
    /* Runs on the esp_timer task, not in an ISR, so calling into Bluedroid is
     * fine. The answer arrives later as a GAP event. */
    const esp_err_t err = esp_bt_gap_read_rssi_delta(s_rssi_peer);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "RSSI request failed: %s", esp_err_to_name(err));
    }
}

void bt_core_link_monitor_start(const uint8_t *bda)
{
    if (bda == NULL) {
        return;
    }
    memcpy(s_rssi_peer, bda, sizeof(s_rssi_peer));
    s_rssi_peer_valid = true;
    s_link.valid = false;

    if (s_rssi_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = rssi_poll_cb,
            .name = "rssi",
            .dispatch_method = ESP_TIMER_TASK,
        };
        if (esp_timer_create(&args, &s_rssi_timer) != ESP_OK) {
            ESP_LOGW(TAG, "link monitor timer create failed");
            return;
        }
    }
    esp_timer_stop(s_rssi_timer);
    esp_timer_start_periodic(s_rssi_timer,
                             (uint64_t)CONFIG_PUCK_RSSI_POLL_SECONDS * 1000000ULL);

    /* Ask immediately so the meter is populated before the first tick. */
    rssi_poll_cb(NULL);
}

void bt_core_link_monitor_stop(void)
{
    if (s_rssi_timer != NULL) {
        esp_timer_stop(s_rssi_timer);
    }
    s_rssi_peer_valid = false;
    s_link.valid = false;
    s_link.bars = 0;
}

void bt_core_link_quality_get(bt_core_link_quality_t *out)
{
    if (out != NULL) {
        *out = s_link;
    }
}

bool bt_core_handle_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event != ESP_BT_GAP_READ_RSSI_DELTA_EVT) {
        return false;
    }

    if (param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS) {
        s_link.rssi_delta = param->read_rssi_delta.rssi_delta;
        s_link.bars = bars_from_delta(s_link.rssi_delta);
        s_link.valid = true;
        ESP_LOGD(TAG, "link rssi delta %d dB -> %u bars", s_link.rssi_delta, s_link.bars);
    } else {
        /* A failed read usually means the link is gone or going. Showing the
         * last good value would be worse than showing nothing. */
        s_link.valid = false;
        s_link.bars = 0;
    }
    return true;
}

int bt_core_bond_count(void)
{
    return esp_bt_gap_get_bond_device_num();
}

esp_err_t bt_core_forget_bonds(void)
{
    const int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) {
        ESP_LOGI(TAG, "no bonded sources to forget");
        return ESP_OK;
    }

    esp_bd_addr_t *list = calloc((size_t)count, sizeof(esp_bd_addr_t));
    ESP_RETURN_ON_FALSE(list != NULL, ESP_ERR_NO_MEM, TAG, "bond list alloc failed");

    int fetched = count;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&fetched, list);
    if (err == ESP_OK) {
        for (int i = 0; i < fetched; i++) {
            esp_bt_gap_remove_bond_device(list[i]);
        }
        ESP_LOGI(TAG, "forgot %d bonded source(s)", fetched);
    }
    free(list);
    return err;
}

const char *bt_core_bda_str(const uint8_t *bda, char *out, size_t size)
{
    if (bda == NULL || out == NULL || size < 18) {
        return NULL;
    }
    snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return out;
}

esp_err_t bt_core_stack_init(void)
{
    /* NVS holds PHY calibration data and the bonded-device link keys. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    /* Classic-only firmware: hand the BLE controller memory back to the heap. */
    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_BLE), TAG,
                        "releasing BLE memory failed");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "controller init failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG,
                        "controller enable failed");

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    /* Secure Simple Pairing, stated rather than assumed. It has no Kconfig
     * symbol -- it is this runtime flag, and it defaults on. With it off the
     * stack falls back to legacy PIN pairing, whose key agreement is far
     * weaker, and which this firmware would fail anyway since it handles no
     * PIN request event. Fail-closed either way, but on purpose now. */
    bluedroid_cfg.ssp_en = true;
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_cfg), TAG,
                        "bluedroid init failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable failed");

    /* Secure Simple Pairing with no input and no output: the puck has no way to
     * show or confirm a passkey, so it must not claim that it does. */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap,
                                                      sizeof(iocap)),
                        TAG, "setting IO capability failed");

    char bda_str[18];
    ESP_LOGI(TAG, "controller up, address %s",
             bt_core_bda_str(esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    return ESP_OK;
}
