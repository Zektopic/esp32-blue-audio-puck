/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * The status screen: what is playing, how loud, and how much battery is left.
 *
 * Two rates, deliberately. Track text and battery are read at 2 Hz -- reading
 * the track takes the AVRCP mutex, and a display has no business holding that
 * often. The marquee animates at 8 Hz from the cached copy, which is fast
 * enough to look smooth and costs nothing but a redraw.
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "puck_avrcp.h"
#include "puck_battery.h"
#include "puck_display.h"
#include "ssd1306.h"

static const char *TAG = "puck_disp";

#define TASK_STACK        4096
#define TASK_PRIO         3
#define FRAME_MS          125   /* 8 Hz: marquee animation */
#define DATA_EVERY_FRAMES 4     /* 2 Hz: track and battery */

#define MARQUEE_PAUSE_FRAMES 8  /* hold at each end before sliding */
#define MARQUEE_STEP_PX      2

typedef struct {
    bool                  present;
    volatile puck_screen_t screen;
    TaskHandle_t          task;
    uint8_t               fb[SSD1306_FB_SIZE];

    /* Cached at the slow rate so the fast redraw touches no locks. */
    puck_track_info_t      track;
    puck_battery_reading_t battery;
    uint8_t                volume;
    bool                   playing;

    int      marquee_px;
    int      marquee_hold;

    char     toast[22];
    uint32_t toast_frames;
} puck_display_t;

static puck_display_t s_disp;

static void refresh_data(void)
{
    puck_avrcp_get_track(&s_disp.track);
    puck_battery_get(&s_disp.battery);
    s_disp.volume = puck_avrcp_get_volume();
    s_disp.playing = puck_avrcp_is_playing();
}

/** Top row: what the link is doing on the left, power on the right. */
static void draw_status_bar(const char *left)
{
    gfx_text(s_disp.fb, 0, 0, left, 1, true);

    if (s_disp.battery.state == PUCK_BATTERY_ABSENT) {
        /* No divider fitted, so there is nothing to report. Saying "USB" is
         * honest; showing a full battery icon would be a lie. */
        gfx_text(s_disp.fb, SSD1306_WIDTH - (3 * FONT_ADVANCE), 0, "USB", 1, true);
    } else if (!s_disp.battery.valid) {
        gfx_text(s_disp.fb, SSD1306_WIDTH - (3 * FONT_ADVANCE), 0, "...", 1, true);
    } else {
        char pct[6];
        snprintf(pct, sizeof(pct), "%u%%", s_disp.battery.percent);
        const int icon_x = SSD1306_WIDTH - 15;
        const int pct_x = icon_x - gfx_text_width(pct, 1) - 3;
        gfx_text(s_disp.fb, pct_x, 0, pct, 1, true);
        gfx_battery(s_disp.fb, icon_x, 0, s_disp.battery.percent);
    }

    gfx_hline(s_disp.fb, 0, 9, SSD1306_WIDTH, true);
}

/** Bottom row: transport state on the left, a volume bar on the right. */
static void draw_volume_row(void)
{
    const int y = SSD1306_HEIGHT - 9;

    gfx_text(s_disp.fb, 0, y, s_disp.playing ? "PLAY" : "PAUSE", 1, true);

    const int bar_x = 40;
    const int bar_w = SSD1306_WIDTH - bar_x - 26;
    gfx_rect(s_disp.fb, bar_x, y, bar_w, 7, true);

    const int fill = (s_disp.volume * (bar_w - 4)) / PUCK_VOLUME_MAX;
    if (fill > 0) {
        gfx_fill_rect(s_disp.fb, bar_x + 2, y + 2, fill, 3, true);
    }

    char pct[6];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)s_disp.volume * 100u / PUCK_VOLUME_MAX);
    gfx_text(s_disp.fb, SSD1306_WIDTH - gfx_text_width(pct, 1), y, pct, 1, true);
}

/**
 * Draw one line of text, scrolling it if it does not fit.
 *
 * Only the line that overflows scrolls, and it pauses at both ends -- text
 * that slides continuously is much harder to read than text that stops.
 */
static void draw_scrolling_line(int y, const char *text, int scale, bool animate)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    const int width = gfx_text_width(text, scale);
    if (width <= SSD1306_WIDTH) {
        gfx_text(s_disp.fb, 0, y, text, scale, true);
        return;
    }

    gfx_text_window(s_disp.fb, 0, y, SSD1306_WIDTH, text, scale, s_disp.marquee_px);

    if (!animate) {
        return;
    }
    const int max_offset = width - SSD1306_WIDTH;
    if (s_disp.marquee_hold > 0) {
        s_disp.marquee_hold--;
    } else if (s_disp.marquee_px >= max_offset) {
        s_disp.marquee_px = 0;
        s_disp.marquee_hold = MARQUEE_PAUSE_FRAMES;
    } else {
        s_disp.marquee_px += MARQUEE_STEP_PX;
        if (s_disp.marquee_px >= max_offset) {
            s_disp.marquee_px = max_offset;
            s_disp.marquee_hold = MARQUEE_PAUSE_FRAMES;
        }
    }
}

static void draw_now_playing(void)
{
    draw_status_bar("BT");

    const char *title = s_disp.track.title[0] ? s_disp.track.title : "(no track info)";
    const char *artist = s_disp.track.artist[0] ? s_disp.track.artist : "";

    draw_scrolling_line(14, title, 2, true);
    if (artist[0] != '\0') {
        gfx_text(s_disp.fb, 0, 34, artist, 1, true);
    }

    draw_volume_row();
}

static void draw_centred(int y, const char *text, int scale)
{
    const int w = gfx_text_width(text, scale);
    gfx_text(s_disp.fb, (SSD1306_WIDTH - w) / 2, y, text, scale, true);
}

static void draw_frame(bool animate)
{
    gfx_clear(s_disp.fb);

    switch (s_disp.screen) {
    case PUCK_SCREEN_PAIRING:
        draw_status_bar("PAIRING");
        draw_centred(22, "Ready to", 1);
        draw_centred(34, "pair", 2);
        break;

    case PUCK_SCREEN_IDLE:
        draw_status_bar("IDLE");
        draw_centred(24, CONFIG_PUCK_BT_DEVICE_NAME, 1);
        draw_centred(40, "hold to pair", 1);
        break;

    case PUCK_SCREEN_NOW_PLAYING:
        draw_now_playing();
        break;

    case PUCK_SCREEN_SPLASH:
    default:
        draw_centred(16, "BlueAudio", 2);
        draw_centred(38, "Puck", 2);
        break;
    }

    /* A toast covers the bottom of whatever is underneath, so a volume change
     * is readable without losing the screen behind it. */
    if (s_disp.toast_frames > 0) {
        s_disp.toast_frames--;
        gfx_fill_rect(s_disp.fb, 0, SSD1306_HEIGHT - 14, SSD1306_WIDTH, 14, false);
        gfx_rect(s_disp.fb, 0, SSD1306_HEIGHT - 14, SSD1306_WIDTH, 14, true);
        draw_centred(SSD1306_HEIGHT - 10, s_disp.toast, 1);
    }

    (void)animate;
}

static void display_task(void *arg)
{
    (void)arg;
    uint32_t frame = 0;

    for (;;) {
        if ((frame % DATA_EVERY_FRAMES) == 0) {
            refresh_data();
        }
        draw_frame(true);

        const esp_err_t err = ssd1306_flush(s_disp.fb);
        if (err != ESP_OK) {
            /* A panel unplugged mid-run should not spin the log or the CPU. */
            ESP_LOGW(TAG, "flush failed: %s; display disabled", esp_err_to_name(err));
            s_disp.present = false;
            vTaskDelete(NULL);
        }

        frame++;
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void puck_display_self_test(void)
{
    if (!s_disp.present) {
        return;
    }

    /* No font involved, on purpose: this separates "the bus and the panel
     * work" from "the text rendering is right". Those have entirely different
     * fixes, and without this step a blank screen does not tell you which. */
    gfx_fill(s_disp.fb);
    ssd1306_flush(s_disp.fb);
    vTaskDelay(pdMS_TO_TICKS(400));

    gfx_clear(s_disp.fb);
    for (int y = 0; y < SSD1306_HEIGHT; y += 8) {
        for (int x = 0; x < SSD1306_WIDTH; x += 8) {
            if (((x / 8) + (y / 8)) % 2 == 0) {
                gfx_fill_rect(s_disp.fb, x, y, 8, 8, true);
            }
        }
    }
    ssd1306_flush(s_disp.fb);
    vTaskDelay(pdMS_TO_TICKS(400));

    /* A border proves the panel geometry is right way round and not offset. */
    gfx_clear(s_disp.fb);
    gfx_rect(s_disp.fb, 0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, true);
    gfx_text(s_disp.fb, 4, 28, "SELF TEST OK", 1, true);
    ssd1306_flush(s_disp.fb);
    vTaskDelay(pdMS_TO_TICKS(600));

    ESP_LOGI(TAG, "self test done");
}

esp_err_t puck_display_init(void)
{
    memset(&s_disp, 0, sizeof(s_disp));
    s_disp.screen = PUCK_SCREEN_SPLASH;

    if (CONFIG_PUCK_I2C_SDA_GPIO < 0 || CONFIG_PUCK_I2C_SCL_GPIO < 0) {
        ESP_LOGI(TAG, "no display fitted");
        return ESP_OK;
    }

    const esp_err_t err = ssd1306_init(CONFIG_PUCK_I2C_SDA_GPIO, CONFIG_PUCK_I2C_SCL_GPIO,
                                       CONFIG_PUCK_DISPLAY_I2C_ADDRESS);
    if (err != ESP_OK) {
        /* A missing screen must not stop a music player from playing music. */
        ESP_LOGW(TAG, "display unavailable (%s); continuing without it",
                 esp_err_to_name(err));
        return ESP_OK;
    }
    s_disp.present = true;

    ESP_RETURN_ON_ERROR(ssd1306_set_contrast(CONFIG_PUCK_DISPLAY_CONTRAST), TAG,
                        "contrast failed");

#if CONFIG_PUCK_DISPLAY_SELF_TEST
    puck_display_self_test();
#endif

    ESP_RETURN_ON_FALSE(xTaskCreate(display_task, "puck_disp", TASK_STACK, NULL,
                                    TASK_PRIO, &s_disp.task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "display task create failed");
    return ESP_OK;
}

bool puck_display_present(void)
{
    return s_disp.present;
}

void puck_display_set_screen(puck_screen_t screen)
{
    if (s_disp.screen == screen) {
        return;
    }
    s_disp.screen = screen;
    /* Restart the marquee so a new track starts from its beginning. */
    s_disp.marquee_px = 0;
    s_disp.marquee_hold = MARQUEE_PAUSE_FRAMES;
}

void puck_display_toast(const char *text, uint32_t ms)
{
    if (text == NULL) {
        return;
    }
    snprintf(s_disp.toast, sizeof(s_disp.toast), "%s", text);
    s_disp.toast_frames = (ms + FRAME_MS - 1) / FRAME_MS;
}

void puck_display_off(void)
{
    if (!s_disp.present) {
        return;
    }
    if (s_disp.task != NULL) {
        vTaskDelete(s_disp.task);
        s_disp.task = NULL;
    }
    gfx_clear(s_disp.fb);
    ssd1306_flush(s_disp.fb);
    ssd1306_set_power(false);
    ESP_LOGI(TAG, "display off");
}
