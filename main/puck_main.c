/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 *
 * SPDX-License-Identifier: MIT
 *
 * BlueAudio Puck -- application entry point.
 *
 * At this stage the firmware only reports what it is running on. The Bluetooth
 * audio path is added in later increments; keeping the banner here gives every
 * later increment a known-good first line of serial output to check against.
 */

#include <inttypes.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "puck";

static void log_boot_banner(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    ESP_LOGI(TAG, "BlueAudio Puck booting (IDF %s)", esp_get_idf_version());
    ESP_LOGI(TAG, "%s rev v%u.%u, %d core(s), flash %" PRIu32 " MB%s",
             CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100,
             chip.cores,
             flash_size / (1024U * 1024U),
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? " (embedded)" : "");

    /* A2DP needs Bluetooth Classic (BR/EDR). The S3/C3 are BLE-only and cannot
     * run this firmware, so say so loudly rather than failing later. */
    if ((chip.features & CHIP_FEATURE_BT) == 0) {
        ESP_LOGE(TAG, "This chip has no Bluetooth Classic -- A2DP is not possible here");
    }

    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
}

void app_main(void)
{
    log_boot_banner();
}
