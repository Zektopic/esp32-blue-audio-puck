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
    uint16_t       event;
    void          *param;
} bt_core_msg_t;

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;

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
        free(msg.param);
    }
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
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue != NULL) {
        /* Drain first: queued messages own heap copies of their parameters. */
        bt_core_msg_t msg;
        while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
            free(msg.param);
        }
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}

bool bt_core_dispatch(bt_core_work_t work, uint16_t event, void *params, int param_len,
                      bt_core_copy_t copy)
{
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "dispatch before task start");
        return false;
    }

    bt_core_msg_t msg = {
        .work  = work,
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

    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "work queue full, dropping event %u", event);
        free(msg.param);
        return false;
    }
    return true;
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
