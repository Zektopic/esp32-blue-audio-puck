/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * Battery sensing: an ADC on a resistor divider, turned into a percentage.
 *
 * Be honest about what this can and cannot do. A percentage derived from
 * terminal voltage is an estimate, not a fuel gauge. Li-po voltage sags under
 * load -- at the ~150 mA this device draws while streaming, the same cell reads
 * materially lower than it does idle -- and the curve between 3.7 V and 3.9 V
 * is nearly flat, which is most of the useful capacity. A real gauge needs
 * coulomb counting (MAX17048 and friends).
 *
 * What this is good for: telling the difference between full, half, and about
 * to die. That is what a status screen needs.
 */

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "puck_battery.h"

static const char *TAG = "puck_batt";

#define SAMPLE_TASK_STACK   3072
#define SAMPLE_TASK_PRIO    3
#define OVERSAMPLE          16    /* raw reads averaged per sample */

/*
 * Li-po discharge curve, open circuit, 10% steps from empty to full.
 *
 * Deliberately a table rather than a formula: the real curve is flat in the
 * middle and steep at both ends, and any linear fit spends most of its life
 * wrong. Interpolating between these points is close enough for a status icon.
 */
static const uint16_t s_curve_mv[11] = {
    3300,  /*   0% -- treat as empty; below this a protection circuit cuts in */
    3600,  /*  10% */
    3700,  /*  20% */
    3730,  /*  30% */
    3770,  /*  40% */
    3790,  /*  50% */
    3820,  /*  60% */
    3870,  /*  70% */
    3920,  /*  80% */
    4000,  /*  90% */
    4200,  /* 100% -- a full cell off the charger */
};

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t         cali;      /*!< NULL if the chip has no eFuse calibration */
    adc_channel_t             channel;
    bool                      fitted;
    TaskHandle_t              task;
    puck_battery_cb_t         cb;
    puck_battery_reading_t    reading;
    int32_t                   filtered_mv;  /*!< smoothed, -1 until the first sample */
} puck_battery_t;

static puck_battery_t s_bat;

/**
 * Map GPIO to an ADC1 channel.
 *
 * ADC1 only, on purpose: ADC2 is shared with the Wi-Fi radio and its readings
 * become unavailable whenever Wi-Fi is active. This firmware does not use
 * Wi-Fi today, but pinning to ADC1 removes the trap in advance.
 */
static int adc1_channel_for_gpio(int gpio)
{
    switch (gpio) {
    case 36: return ADC_CHANNEL_0;
    case 37: return ADC_CHANNEL_1;
    case 38: return ADC_CHANNEL_2;
    case 39: return ADC_CHANNEL_3;
    case 32: return ADC_CHANNEL_4;
    case 33: return ADC_CHANNEL_5;
    case 34: return ADC_CHANNEL_6;
    case 35: return ADC_CHANNEL_7;
    default: return -1;
    }
}

static uint8_t percent_from_mv(uint16_t mv)
{
    if (mv <= s_curve_mv[0]) {
        return 0;
    }
    if (mv >= s_curve_mv[10]) {
        return 100;
    }
    for (int i = 1; i <= 10; i++) {
        if (mv < s_curve_mv[i]) {
            const uint16_t lo = s_curve_mv[i - 1];
            const uint16_t hi = s_curve_mv[i];
            const uint32_t within = ((uint32_t)(mv - lo) * 10u) / (hi - lo);
            return (uint8_t)((i - 1) * 10u + within);
        }
    }
    return 100;
}

static puck_battery_state_t state_from_percent(uint8_t percent)
{
    if (percent <= CONFIG_PUCK_BATTERY_CRITICAL_PERCENT) {
        return PUCK_BATTERY_CRITICAL;
    }
    if (percent <= CONFIG_PUCK_BATTERY_LOW_PERCENT) {
        return PUCK_BATTERY_LOW;
    }
    return PUCK_BATTERY_OK;
}

/** One oversampled reading, in millivolts at the ADC pin. */
static bool read_pin_mv(int *out_mv)
{
    int32_t total = 0;
    int taken = 0;

    for (int i = 0; i < OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_bat.unit, s_bat.channel, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s_bat.cali != NULL) {
            if (adc_cali_raw_to_voltage(s_bat.cali, raw, &mv) != ESP_OK) {
                continue;
            }
        } else {
            /* No eFuse calibration on this chip. 12-bit over the 11 dB
             * attenuation range is a rough nominal, and worth saying so
             * rather than pretending the number is trustworthy. */
            mv = (raw * 3100) / 4095;
        }
        total += mv;
        taken++;
    }

    if (taken == 0) {
        return false;
    }
    *out_mv = (int)(total / taken);
    return true;
}

static void sample_once(void)
{
    int pin_mv = 0;
    if (!read_pin_mv(&pin_mv)) {
        ESP_LOGW(TAG, "ADC read failed");
        return;
    }

    /* Undo the divider. Ratio is expressed in hundredths so a 2:1 divider is
     * 200 and odd resistor pairs stay expressible without floats. */
    const int32_t cell_mv = ((int32_t)pin_mv * CONFIG_PUCK_BATTERY_DIVIDER_RATIO_X100) / 100;

    /* Exponential smoothing. Playback current draw makes the terminal voltage
     * jump around; without this the percentage flickers on screen. */
    if (s_bat.filtered_mv < 0) {
        s_bat.filtered_mv = cell_mv;
    } else {
        s_bat.filtered_mv += (cell_mv - s_bat.filtered_mv) / 4;
    }

    const puck_battery_state_t was = s_bat.reading.state;

    s_bat.reading.millivolts = (uint16_t)s_bat.filtered_mv;
    s_bat.reading.percent = percent_from_mv(s_bat.reading.millivolts);
    s_bat.reading.state = state_from_percent(s_bat.reading.percent);
    s_bat.reading.valid = true;

    if (s_bat.reading.state != was) {
        ESP_LOGI(TAG, "%u mV, %u%%, state %d", s_bat.reading.millivolts,
                 s_bat.reading.percent, s_bat.reading.state);
        if (s_bat.cb) {
            s_bat.cb(&s_bat.reading);
        }
    }
}

static void sample_task(void *arg)
{
    (void)arg;

    for (;;) {
        sample_once();
        /* A notification cuts the wait short when someone wants a reading
         * immediately, e.g. the display waking up. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_PUCK_BATTERY_INTERVAL_SECONDS * 1000));
    }
}

esp_err_t puck_battery_init(void)
{
    memset(&s_bat, 0, sizeof(s_bat));
    s_bat.filtered_mv = -1;
    s_bat.reading.state = PUCK_BATTERY_ABSENT;

    const int gpio = CONFIG_PUCK_BATTERY_ADC_GPIO;
    if (gpio < 0) {
        ESP_LOGI(TAG, "no battery sense fitted; assuming USB power");
        return ESP_OK;
    }

    const int channel = adc1_channel_for_gpio(gpio);
    ESP_RETURN_ON_FALSE(channel >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "GPIO %d is not an ADC1 pin (use 32-39)", gpio);
    s_bat.channel = (adc_channel_t)channel;

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_bat.unit), TAG,
                        "ADC unit init failed");

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,     /* full-scale ~3.1 V, so a divided cell fits */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_bat.unit, s_bat.channel, &chan_cfg), TAG,
                        "ADC channel config failed");

    /* Line fitting is the ESP32's scheme; curve fitting is S3-only. It needs
     * eFuse calibration data, which not every chip has -- so this is allowed
     * to fail, and readings fall back to a nominal scaling. */
    const adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_bat.cali) != ESP_OK) {
        s_bat.cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration on this chip; readings are approximate");
    }

    s_bat.fitted = true;
    s_bat.reading.state = PUCK_BATTERY_OK;

    ESP_RETURN_ON_FALSE(xTaskCreate(sample_task, "puck_batt", SAMPLE_TASK_STACK, NULL,
                                    SAMPLE_TASK_PRIO, &s_bat.task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "sample task create failed");

    ESP_LOGI(TAG, "sensing on GPIO %d, divider %d.%02dx, every %ds",
             gpio, CONFIG_PUCK_BATTERY_DIVIDER_RATIO_X100 / 100,
             CONFIG_PUCK_BATTERY_DIVIDER_RATIO_X100 % 100,
             CONFIG_PUCK_BATTERY_INTERVAL_SECONDS);
    return ESP_OK;
}

void puck_battery_get(puck_battery_reading_t *out)
{
    if (out == NULL) {
        return;
    }
    /* Every field is written by the sampling task and read here; on this
     * target these are aligned single-word stores that cannot tear, and a
     * mutex on a display refresh path would buy nothing. */
    *out = s_bat.reading;
}

void puck_battery_set_cb(puck_battery_cb_t cb)
{
    s_bat.cb = cb;
}

void puck_battery_sample_now(void)
{
    if (s_bat.task != NULL) {
        xTaskNotifyGive(s_bat.task);
    }
}
