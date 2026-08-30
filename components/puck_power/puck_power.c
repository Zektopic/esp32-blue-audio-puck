/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * Power policy.
 *
 * The honest framing: on this hardware the ESP32 with Bluetooth Classic
 * active is roughly three quarters of the current budget, and none of the
 * levers here change that. What they do change is the two things that are
 * actually within reach in firmware -- the radio's transmit power, and what
 * the puck does with the hours it spends in a bag with nothing connected.
 *
 * Deep sleep does not extend playback by a second. It is the difference
 * between "flat by morning" and "fine next week", which on a device like this
 * matters more.
 */

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "puck_power.h"

static const char *TAG = "puck_power";

typedef struct {
    esp_timer_handle_t    idle_timer;
    puck_power_activity_t activity;
    bool                  woke_from_sleep;
} puck_power_t;

static puck_power_t s_pwr;

static void idle_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "idle for %d minutes, shutting down", CONFIG_PUCK_IDLE_SLEEP_MINUTES);
    puck_power_sleep_now();
}

static esp_err_t configure_radio(void)
{
    /* Transmit power. The source is usually in the same pocket, and the top of
     * the range costs current for range nobody is using. Kept as a Kconfig
     * knob because a puck in a bag with the phone across the room is a real
     * case that wants the opposite trade. */
    const esp_power_level_t max_level = (esp_power_level_t)CONFIG_PUCK_BT_TX_POWER_LEVEL;
    ESP_RETURN_ON_ERROR(esp_bredr_tx_power_set(ESP_PWR_LVL_N12, max_level), TAG,
                        "setting BR/EDR transmit power failed");

    /* Let the controller sleep between BR/EDR frames. Enabled by default in
     * the controller config, but asking explicitly means a config change
     * cannot silently turn it off. */
    esp_err_t err = esp_bt_sleep_enable();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "controller modem sleep unavailable in this build");
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "enabling controller sleep failed");
    }

    esp_power_level_t min_read, max_read;
    if (esp_bredr_tx_power_get(&min_read, &max_read) == ESP_OK) {
        ESP_LOGI(TAG, "radio transmit power levels %d..%d", min_read, max_read);
    }
    return ESP_OK;
}

static esp_err_t configure_clocks(void)
{
#if CONFIG_PM_ENABLE
    /* Dynamic frequency scaling. SBC decode plus a few biquads fits
     * comfortably below the maximum, so the CPU drops to the floor frequency
     * whenever nothing needs it.
     *
     * Light sleep stays off deliberately: the controller keeps its reference
     * clock on the main crystal, which supports DFS but not light sleep, and
     * light sleep would break audio timing anyway. */
    const esp_pm_config_t pm = {
        .max_freq_mhz       = CONFIG_PUCK_CPU_MAX_FREQ_MHZ,
        .min_freq_mhz       = CONFIG_PUCK_CPU_MIN_FREQ_MHZ,
        .light_sleep_enable = false,
    };
    ESP_RETURN_ON_ERROR(esp_pm_configure(&pm), TAG, "frequency scaling config failed");
    ESP_LOGI(TAG, "CPU scales between %d and %d MHz",
             CONFIG_PUCK_CPU_MIN_FREQ_MHZ, CONFIG_PUCK_CPU_MAX_FREQ_MHZ);
#else
    ESP_LOGI(TAG, "frequency scaling disabled at build time");
#endif
    return ESP_OK;
}

esp_err_t puck_power_init(void)
{
    s_pwr.woke_from_sleep = (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED);
    if (s_pwr.woke_from_sleep) {
        ESP_LOGI(TAG, "woke from deep sleep");
    }

    ESP_RETURN_ON_ERROR(configure_radio(), TAG, "radio power setup failed");
    ESP_RETURN_ON_ERROR(configure_clocks(), TAG, "clock setup failed");

    if (CONFIG_PUCK_IDLE_SLEEP_MINUTES > 0) {
        const esp_timer_create_args_t args = {
            .callback = idle_timeout_cb,
            .name     = "puck_idle",
            /* Runs on the esp_timer task, not in an ISR: it goes on to shut
             * the Bluetooth controller down, which must not happen in
             * interrupt context. */
            .dispatch_method = ESP_TIMER_TASK,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_pwr.idle_timer), TAG,
                            "idle timer create failed");
    }

    s_pwr.activity = PUCK_POWER_IDLE;
    puck_power_kick();
    return ESP_OK;
}

void puck_power_set_activity(puck_power_activity_t activity)
{
    if (s_pwr.activity == activity) {
        return;
    }
    s_pwr.activity = activity;

    if (s_pwr.idle_timer == NULL) {
        return;
    }

    if (activity == PUCK_POWER_IDLE) {
        puck_power_kick();
    } else {
        /* Something is connected. Stop counting down -- a paused phone in a
         * pocket should not have the puck disappear out from under it. */
        esp_timer_stop(s_pwr.idle_timer);
        ESP_LOGD(TAG, "idle shutdown cancelled");
    }
}

void puck_power_kick(void)
{
    if (s_pwr.idle_timer == NULL || s_pwr.activity != PUCK_POWER_IDLE) {
        return;
    }
    esp_timer_stop(s_pwr.idle_timer);
    const uint64_t timeout_us = (uint64_t)CONFIG_PUCK_IDLE_SLEEP_MINUTES * 60ULL * 1000000ULL;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(s_pwr.idle_timer, timeout_us));
    ESP_LOGD(TAG, "idle shutdown in %d minutes", CONFIG_PUCK_IDLE_SLEEP_MINUTES);
}

void puck_power_sleep_now(void)
{
    ESP_LOGI(TAG, "entering deep sleep, press the button to wake");

    /* Bring the radio down cleanly. A controller left running holds power
     * domains up and would undo the point of the exercise. */
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    if (CONFIG_PUCK_BUTTON_GPIO >= 0) {
        /* ext0 wakes on a level, and the button pulls its pin low. The RTC
         * pull-up has to be asked for separately: the digital pull-up
         * configured by the UI code does not survive into deep sleep, and
         * without it the pin floats and the chip wakes immediately. */
        const gpio_num_t wake_pin = (gpio_num_t)CONFIG_PUCK_BUTTON_GPIO;
        if (esp_sleep_enable_ext0_wakeup(wake_pin, 0) == ESP_OK) {
            rtc_gpio_pullup_en(wake_pin);
            rtc_gpio_pulldown_dis(wake_pin);
        } else {
            ESP_LOGE(TAG, "GPIO %d cannot wake from deep sleep -- not an RTC pin",
                     CONFIG_PUCK_BUTTON_GPIO);
        }
    } else {
        ESP_LOGW(TAG, "no button fitted: only a reset will wake the puck");
    }

    esp_deep_sleep_start();
}

bool puck_power_woke_from_sleep(void)
{
    return s_pwr.woke_from_sleep;
}
