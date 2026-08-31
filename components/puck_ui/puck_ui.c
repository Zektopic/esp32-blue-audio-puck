/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * Physical interface: three front-panel buttons and a status LED.
 *
 * Each button is an independent state machine reporting taps, holds and
 * repeats. What any of that *means* lives in the application -- this file has
 * no idea what a track is.
 *
 * The UI task sleeps indefinitely when nothing is happening and is woken by a
 * GPIO interrupt, rather than polling on a timer. On a device that will later
 * want deep sleep, a task that ticks 50 times a second forever is exactly the
 * thing that stops it.
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "puck_ui.h"

static const char *TAG = "puck_ui";

#define POLL_INTERVAL_MS     20    /* only while a button is down or the LED animates */
#define DEBOUNCE_MS          30

#define LED_TIMER            LEDC_TIMER_0
#define LED_CHANNEL          LEDC_CHANNEL_0
#define LED_RESOLUTION       LEDC_TIMER_10_BIT
#define LED_DUTY_MAX         ((1 << 10) - 1)
#define LED_FREQ_HZ          5000

#define UI_TASK_STACK        3072
#define UI_TASK_PRIO         4

/**
 * Timing, per button.
 *
 * Deliberately not uniform. A button that steps the volume wants a short hold
 * threshold and a brisk repeat, because the user is watching a number move. A
 * button that opens pairing wants a long one, because doing it by accident is
 * annoying and there is nothing to watch. One shared threshold would have to
 * be wrong for one of them.
 *
 * hold_ms 0 disables holding entirely; repeat_ms 0 means fire once and stop;
 * extra_ms 0 means there is no second tier.
 */
typedef struct {
    int         gpio;
    uint32_t    hold_ms;
    uint32_t    repeat_ms;
    uint32_t    extra_ms;
    const char *name;
} button_cfg_t;

static const button_cfg_t s_cfg[PUCK_UI_BUTTON_COUNT] = {
    {CONFIG_PUCK_BT1_GPIO,  500, 180,    0, "BT1"},
    {CONFIG_PUCK_BT2_GPIO, 1500,   0, 5000, "BT2"},
    {CONFIG_PUCK_BT3_GPIO,  500, 180,    0, "BT3"},
};

typedef struct {
    bool    down;
    int64_t down_at_us;
    int64_t last_release_us;
    int64_t next_repeat_us;
    bool    hold_fired;   /*!< hold already reported for this press */
    bool    extra_fired;  /*!< extra-long already reported for this press */
} button_state_t;

typedef struct {
    TaskHandle_t        task;
    puck_ui_button_cb_t button_cb;
    volatile puck_ui_state_t state;

    button_state_t buttons[PUCK_UI_BUTTON_COUNT];

    /* LED animation phase, advanced once per poll tick */
    uint16_t led_phase;
} puck_ui_t;

static puck_ui_t s_ui;

static inline bool button_pressed(int gpio)
{
    /* Active low: the pin idles high through its pull-up. */
    return gpio >= 0 && gpio_get_level((gpio_num_t)gpio) == 0;
}

static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (s_ui.task != NULL) {
        vTaskNotifyGiveFromISR(s_ui.task, &higher_priority_task_woken);
    }
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static const char *event_name(puck_ui_event_t event)
{
    switch (event) {
    case PUCK_UI_TAP:         return "tap";
    case PUCK_UI_HOLD:        return "hold";
    case PUCK_UI_HOLD_REPEAT: return "hold-repeat";
    case PUCK_UI_HOLD_EXTRA:  return "hold-extra";
    default:                  return "?";
    }
}

static void emit(puck_ui_button_t button, puck_ui_event_t event)
{
    /* Repeats are debug-level: at 180 ms they would otherwise flood the log
     * for as long as someone holds the volume down. */
    if (event == PUCK_UI_HOLD_REPEAT) {
        ESP_LOGD(TAG, "%s %s", s_cfg[button].name, event_name(event));
    } else {
        ESP_LOGI(TAG, "%s %s", s_cfg[button].name, event_name(event));
    }
    if (s_ui.button_cb) {
        s_ui.button_cb(button, event);
    }
}

static void led_set(uint32_t duty)
{
    if (CONFIG_PUCK_LED_GPIO < 0) {
        return;
    }
#if CONFIG_PUCK_LED_ACTIVE_LOW
    duty = LED_DUTY_MAX - duty;
#endif
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL);
}

/**
 * Advance the LED animation by one tick.
 *
 * Called at POLL_INTERVAL_MS while anything is animating. Steady states set
 * their duty and return false so the task can go back to sleep instead of
 * waking 50 times a second to write the same value.
 *
 * @return true if this state needs further ticks.
 */
static bool led_tick(void)
{
    s_ui.led_phase++;

    switch (s_ui.state) {
    case PUCK_UI_PAIRING:
        /* ~3 Hz blink: unmistakably "waiting for you". */
        led_set((s_ui.led_phase % 16 < 8) ? LED_DUTY_MAX : 0);
        return true;

    case PUCK_UI_CONNECTED: {
        /* Triangle breathe over about three seconds. */
        const uint16_t period = 150;
        const uint16_t pos = s_ui.led_phase % period;
        const uint16_t half = period / 2;
        const uint32_t level = (pos < half) ? pos : (period - pos);
        led_set((level * LED_DUTY_MAX) / half / 3);
        return true;
    }

    case PUCK_UI_FAULT:
        /* Three rapid flashes, then a pause. */
        led_set((s_ui.led_phase % 50 < 18 && (s_ui.led_phase % 6 < 3)) ? LED_DUTY_MAX : 0);
        return true;

    case PUCK_UI_PLAYING:
        led_set(LED_DUTY_MAX / 3);
        return false;

    case PUCK_UI_BOOTING:
    default:
        led_set(LED_DUTY_MAX / 12);
        return false;
    }
}

/**
 * Advance one button.
 *
 * @return true while this button is still down, so the caller keeps polling
 *         instead of going back to sleep.
 */
static bool service_button(puck_ui_button_t index, int64_t now_us)
{
    const button_cfg_t *cfg = &s_cfg[index];
    button_state_t *st = &s_ui.buttons[index];

    if (cfg->gpio < 0) {
        return false;
    }

    const bool pressed = button_pressed(cfg->gpio);

    if (pressed && !st->down) {
        /* Ignore a re-assert inside the debounce window: contact bounce on
         * release would otherwise read as a fresh press. */
        if (now_us - st->last_release_us < DEBOUNCE_MS * 1000) {
            return true;
        }
        st->down = true;
        st->down_at_us = now_us;
        st->hold_fired = false;
        st->extra_fired = false;
        st->next_repeat_us = 0;
        return true;
    }

    if (pressed && st->down) {
        const int64_t held_us = now_us - st->down_at_us;

        if (cfg->hold_ms && !st->hold_fired &&
            held_us >= (int64_t)cfg->hold_ms * 1000) {
            /* Fire on crossing the threshold, not on release: the user gets
             * feedback while still holding, which is what makes a hold feel
             * like a hold rather than a slow tap. */
            st->hold_fired = true;
            st->next_repeat_us = now_us + (int64_t)cfg->repeat_ms * 1000;
            emit(index, PUCK_UI_HOLD);
        } else if (st->hold_fired && cfg->repeat_ms &&
                   now_us >= st->next_repeat_us) {
            st->next_repeat_us = now_us + (int64_t)cfg->repeat_ms * 1000;
            emit(index, PUCK_UI_HOLD_REPEAT);
        }

        if (cfg->extra_ms && !st->extra_fired &&
            held_us >= (int64_t)cfg->extra_ms * 1000) {
            st->extra_fired = true;
            emit(index, PUCK_UI_HOLD_EXTRA);
        }
        return true;
    }

    if (!pressed && st->down) {
        st->down = false;
        st->last_release_us = now_us;
        /* A tap only counts if nothing longer already fired, and if the press
         * outlasted the debounce window. */
        if (!st->hold_fired && (now_us - st->down_at_us) >= DEBOUNCE_MS * 1000) {
            emit(index, PUCK_UI_TAP);
        }
        return false;
    }

    return false;
}

static void ui_task(void *arg)
{
    (void)arg;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();

        bool button_busy = false;
        for (int i = 0; i < PUCK_UI_BUTTON_COUNT; i++) {
            /* Not short-circuited: every button has to be serviced on every
             * tick, or holding one would freeze the others. */
            button_busy |= service_button((puck_ui_button_t)i, now_us);
        }
        const bool led_busy = led_tick();

        if (button_busy || led_busy) {
            /* Something is in flight: keep a slow poll going. Clear any
             * notification that arrived meanwhile so it does not cause an
             * immediate spurious wake on the next idle sleep. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_INTERVAL_MS));
        } else {
            /* Nothing to do. Sleep until a button edge or a state change. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

static esp_err_t configure_button(int gpio, const char *what)
{
    if (gpio < 0) {
        ESP_LOGI(TAG, "%s button not fitted", what);
        return ESP_OK;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        /* Both edges: press wakes the task, release lets it finish the
         * gesture without having to poll through the whole hold. */
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "%s button gpio_config failed", what);
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add((gpio_num_t)gpio, button_isr, NULL), TAG,
                        "%s button ISR failed", what);
    ESP_LOGI(TAG, "%s button on GPIO %d", what, gpio);
    return ESP_OK;
}

static esp_err_t configure_led(void)
{
    if (CONFIG_PUCK_LED_GPIO < 0) {
        ESP_LOGI(TAG, "status LED not fitted");
        return ESP_OK;
    }

    const ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LED_RESOLUTION,
        .timer_num       = LED_TIMER,
        .freq_hz         = LED_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LED timer config failed");

    const ledc_channel_config_t channel = {
        .gpio_num   = CONFIG_PUCK_LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LED_CHANNEL,
        .timer_sel  = LED_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "LED channel config failed");
    ESP_LOGI(TAG, "status LED on GPIO %d", CONFIG_PUCK_LED_GPIO);
    return ESP_OK;
}

esp_err_t puck_ui_init(void)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.state = PUCK_UI_BOOTING;

    ESP_RETURN_ON_ERROR(configure_led(), TAG, "LED setup failed");

    /* Shared service: other components may want GPIO interrupts too, and
     * installing it twice is an error rather than a no-op. */
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "GPIO ISR service failed");
    }

    ESP_RETURN_ON_FALSE(xTaskCreate(ui_task, "puck_ui", UI_TASK_STACK, NULL, UI_TASK_PRIO,
                                    &s_ui.task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "UI task create failed");

    /* Buttons last: the ISR notifies s_ui.task, which must already exist. */
    for (int i = 0; i < PUCK_UI_BUTTON_COUNT; i++) {
        ESP_RETURN_ON_ERROR(configure_button(s_cfg[i].gpio, s_cfg[i].name), TAG,
                            "button setup");
    }

    return ESP_OK;
}

void puck_ui_set_state(puck_ui_state_t state)
{
    if (s_ui.state == state) {
        return;
    }
    s_ui.state = state;
    s_ui.led_phase = 0;
    if (s_ui.task != NULL) {
        /* Wake the task so the LED changes now rather than on the next edge. */
        xTaskNotifyGive(s_ui.task);
    }
}

puck_ui_state_t puck_ui_get_state(void)
{
    return s_ui.state;
}

void puck_ui_set_button_cb(puck_ui_button_cb_t cb)
{
    s_ui.button_cb = cb;
}
