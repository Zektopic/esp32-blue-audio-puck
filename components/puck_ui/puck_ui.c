/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * Physical interface: one multi-function button, two optional volume buttons,
 * and a status LED.
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

#define POLL_INTERVAL_MS     20    /* while a button is down or a gesture is open */
#define DEBOUNCE_MS          30
#define LONG_PRESS_MS        1500
#define VERY_LONG_PRESS_MS   5000
#define MULTI_CLICK_GAP_MS   350   /* window to wait for another click */
#define VOLUME_REPEAT_MS     220   /* auto-repeat while a volume button is held */

#define LED_TIMER            LEDC_TIMER_0
#define LED_CHANNEL          LEDC_CHANNEL_0
#define LED_RESOLUTION       LEDC_TIMER_10_BIT
#define LED_DUTY_MAX         ((1 << 10) - 1)
#define LED_FREQ_HZ          5000

#define UI_TASK_STACK        3072
#define UI_TASK_PRIO         4

typedef struct {
    TaskHandle_t         task;
    puck_ui_gesture_cb_t gesture_cb;
    volatile puck_ui_state_t state;

    /* main button */
    bool     down;
    int64_t  down_at_us;
    int64_t  last_release_us;
    uint8_t  click_count;
    bool     long_fired;      /*!< long press already reported for this hold */
    bool     very_long_fired; /*!< very long press already reported for this hold */

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

static void emit(puck_ui_gesture_t gesture)
{
    ESP_LOGI(TAG, "gesture %d", gesture);
    if (s_ui.gesture_cb) {
        s_ui.gesture_cb(gesture);
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

/** Resolve an accumulated click count into a gesture. */
static void flush_clicks(void)
{
    switch (s_ui.click_count) {
    case 0:
        break;
    case 1:
        emit(PUCK_UI_PRESS_SINGLE);
        break;
    case 2:
        emit(PUCK_UI_PRESS_DOUBLE);
        break;
    default:
        emit(PUCK_UI_PRESS_TRIPLE);
        break;
    }
    s_ui.click_count = 0;
}

/**
 * Service the main button.
 *
 * @return true while a gesture is still in progress, so the caller keeps
 *         polling instead of sleeping.
 */
static bool service_main_button(int64_t now_us)
{
    const bool pressed = button_pressed(CONFIG_PUCK_BUTTON_GPIO);

    if (pressed && !s_ui.down) {
        /* Ignore a re-assert inside the debounce window: contact bounce would
         * otherwise register as a double click. */
        if (now_us - s_ui.last_release_us < DEBOUNCE_MS * 1000) {
            return true;
        }
        s_ui.down = true;
        s_ui.down_at_us = now_us;
        s_ui.long_fired = false;
        s_ui.very_long_fired = false;
        return true;
    }

    if (pressed && s_ui.down) {
        const int64_t held_us = now_us - s_ui.down_at_us;

        if (!s_ui.long_fired && held_us >= LONG_PRESS_MS * 1000) {
            /* Fire on reaching the threshold, not on release: the user gets
               feedback while still holding, which is what makes it feel like a
               long press rather than a slow click. */
            s_ui.long_fired = true;
            s_ui.click_count = 0;
            emit(PUCK_UI_PRESS_LONG);
        }
        if (!s_ui.very_long_fired && held_us >= VERY_LONG_PRESS_MS * 1000) {
            s_ui.very_long_fired = true;
            emit(PUCK_UI_PRESS_VERY_LONG);
        }
        return true;
    }

    if (!pressed && s_ui.down) {
        s_ui.down = false;
        s_ui.last_release_us = now_us;
        if (!s_ui.long_fired && (now_us - s_ui.down_at_us) >= DEBOUNCE_MS * 1000) {
            s_ui.click_count++;
        }
        return true;
    }

    /* Released and settled: wait out the multi-click window, then resolve. */
    if (s_ui.click_count > 0) {
        if (now_us - s_ui.last_release_us >= MULTI_CLICK_GAP_MS * 1000) {
            flush_clicks();
            return false;
        }
        return true;
    }
    return false;
}

/**
 * Service the optional volume buttons, which auto-repeat while held.
 *
 * @return true while either is held.
 */
static bool service_volume_buttons(int64_t now_us)
{
    static int64_t next_repeat_us;
    bool active = false;

    if (button_pressed(CONFIG_PUCK_VOLUME_UP_GPIO)) {
        active = true;
        if (now_us >= next_repeat_us) {
            emit(PUCK_UI_VOLUME_UP);
            next_repeat_us = now_us + VOLUME_REPEAT_MS * 1000;
        }
    } else if (button_pressed(CONFIG_PUCK_VOLUME_DOWN_GPIO)) {
        active = true;
        if (now_us >= next_repeat_us) {
            emit(PUCK_UI_VOLUME_DOWN);
            next_repeat_us = now_us + VOLUME_REPEAT_MS * 1000;
        }
    } else {
        /* Reset so the next press acts immediately rather than waiting out a
         * repeat interval left over from the last one. */
        next_repeat_us = 0;
    }
    return active;
}

static void ui_task(void *arg)
{
    (void)arg;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();

        const bool button_busy = service_main_button(now_us);
        const bool volume_busy = service_volume_buttons(now_us);
        const bool led_busy = led_tick();

        if (button_busy || volume_busy || led_busy) {
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
    ESP_RETURN_ON_ERROR(configure_button(CONFIG_PUCK_BUTTON_GPIO, "main"), TAG, "button setup");
    ESP_RETURN_ON_ERROR(configure_button(CONFIG_PUCK_VOLUME_UP_GPIO, "volume up"), TAG, "button setup");
    ESP_RETURN_ON_ERROR(configure_button(CONFIG_PUCK_VOLUME_DOWN_GPIO, "volume down"), TAG, "button setup");

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

void puck_ui_set_gesture_cb(puck_ui_gesture_cb_t cb)
{
    s_ui.gesture_cb = cb;
}
