/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include "ssd1306.h"

static const char *TAG = "ssd1306";

/* Control bytes that prefix an I2C payload: command stream or data stream. */
#define CTRL_CMD   0x00
#define CTRL_DATA  0x40

#define I2C_TIMEOUT_MS 100

extern const uint8_t g_font5x7[95][5];

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static esp_err_t send_cmds(const uint8_t *cmds, size_t len)
{
    /* Small and bounded, so a stack buffer avoids a malloc per call. */
    uint8_t buf[24];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, len);
    return i2c_master_transmit(s_dev, buf, len + 1, I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_init(int sda_gpio, int scl_gpio, uint8_t address)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Internal pull-ups are weak (~45k). They are enough for a short
         * jumper run at 400 kHz, and most OLED breakouts fit their own 4.7k
         * pull-ups anyway. A long or flaky bus wants real resistors. */
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus create failed");

    /* Probe before configuring anything: a panel that is absent, mis-wired or
     * strapped to the other address should produce one clear message rather
     * than a cascade of transmit timeouts. */
    if (i2c_master_probe(s_bus, address, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no device at 0x%02x on SDA=%d SCL=%d", address, sda_gpio, scl_gpio);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG,
                        "I2C device add failed");

    static const uint8_t init_seq[] = {
        0xAE,              /* display off while we configure                */
        0xD5, 0x80,        /* clock divide / oscillator frequency           */
        0xA8, 0x3F,        /* multiplex ratio: 64 rows                      */
        0xD3, 0x00,        /* no display offset                             */
        0x40,              /* start line 0                                  */
        0x8D, 0x14,        /* charge pump on -- without this the panel is
                            * powered but stays dark, which reads as a dead
                            * display                                        */
        0x20, 0x00,        /* horizontal addressing: the framebuffer streams
                            * out in one write                               */
        0xA1,              /* segment remap, so column 0 is on the left      */
        0xC8,              /* COM scan descending, so row 0 is at the top    */
        0xDA, 0x12,        /* COM pin layout for a 128x64 panel              */
        0x81, 0x7F,        /* contrast                                       */
        0xD9, 0xF1,        /* pre-charge period                              */
        0xDB, 0x40,        /* VCOMH deselect level                           */
        0xA4,              /* follow RAM, not all-on                         */
        0xA6,              /* normal, not inverted                           */
        0x2E,              /* scrolling off                                  */
    };

    /* Sent in chunks: send_cmds uses a small stack buffer on purpose. */
    for (size_t i = 0; i < sizeof(init_seq); i += 16) {
        const size_t n = (sizeof(init_seq) - i < 16) ? (sizeof(init_seq) - i) : 16;
        ESP_RETURN_ON_ERROR(send_cmds(&init_seq[i], n), TAG, "init sequence failed");
    }

    ESP_LOGI(TAG, "128x64 panel at 0x%02x on SDA=%d SCL=%d", address, sda_gpio, scl_gpio);
    return ssd1306_set_power(true);
}

void ssd1306_deinit(void)
{
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

esp_err_t ssd1306_flush(const uint8_t *fb)
{
    if (s_dev == NULL || fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t window[] = {
        0x21, 0x00, SSD1306_WIDTH - 1,   /* column range */
        0x22, 0x00, SSD1306_PAGES - 1,   /* page range   */
    };
    ESP_RETURN_ON_ERROR(send_cmds(window, sizeof(window)), TAG, "addressing failed");

    /* One transfer for the whole frame. At 400 kHz this is about 21 ms, which
     * is why the refresh task runs at a few hertz and not at video rates. */
    static uint8_t tx[1 + SSD1306_FB_SIZE];
    tx[0] = CTRL_DATA;
    memcpy(&tx[1], fb, SSD1306_FB_SIZE);
    return i2c_master_transmit(s_dev, tx, sizeof(tx), I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_set_power(bool on)
{
    const uint8_t cmd = on ? 0xAF : 0xAE;
    return send_cmds(&cmd, 1);
}

esp_err_t ssd1306_set_contrast(uint8_t contrast)
{
    const uint8_t cmds[] = {0x81, contrast};
    return send_cmds(cmds, sizeof(cmds));
}

/* ---- Drawing ------------------------------------------------------------- */

void gfx_clear(uint8_t *fb)
{
    memset(fb, 0x00, SSD1306_FB_SIZE);
}

void gfx_fill(uint8_t *fb)
{
    memset(fb, 0xFF, SSD1306_FB_SIZE);
}

void gfx_pixel(uint8_t *fb, int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    /* Horizontal addressing: one byte spans eight rows of a single column. */
    uint8_t *cell = &fb[(y / 8) * SSD1306_WIDTH + x];
    const uint8_t mask = 1u << (y % 8);
    if (on) {
        *cell |= mask;
    } else {
        *cell &= (uint8_t)~mask;
    }
}

void gfx_hline(uint8_t *fb, int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) {
        gfx_pixel(fb, x + i, y, on);
    }
}

void gfx_vline(uint8_t *fb, int x, int y, int h, bool on)
{
    for (int i = 0; i < h; i++) {
        gfx_pixel(fb, x, y + i, on);
    }
}

void gfx_rect(uint8_t *fb, int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    gfx_hline(fb, x, y, w, on);
    gfx_hline(fb, x, y + h - 1, w, on);
    gfx_vline(fb, x, y, h, on);
    gfx_vline(fb, x + w - 1, y, h, on);
}

void gfx_fill_rect(uint8_t *fb, int x, int y, int w, int h, bool on)
{
    for (int row = 0; row < h; row++) {
        gfx_hline(fb, x, y + row, w, on);
    }
}

int gfx_char(uint8_t *fb, int x, int y, char c, int scale, bool on)
{
    if (scale < 1) {
        scale = 1;
    }
    /* Anything outside the printable range becomes '?', so a stray byte from a
     * phone's metadata shows as a visible placeholder rather than reading past
     * the table. */
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    const uint8_t *glyph = g_font5x7[(uint8_t)c - 0x20];

    for (int col = 0; col < 5; col++) {
        const uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_HEIGHT; row++) {
            if ((bits >> row) & 1u) {
                if (scale == 1) {
                    gfx_pixel(fb, x + col, y + row, on);
                } else {
                    gfx_fill_rect(fb, x + col * scale, y + row * scale, scale, scale, on);
                }
            }
        }
    }
    return FONT_ADVANCE * scale;
}

int gfx_text(uint8_t *fb, int x, int y, const char *s, int scale, bool on)
{
    int cursor = x;

    for (; s != NULL && *s != '\0'; s++) {
        if (cursor >= SSD1306_WIDTH) {
            break;
        }
        cursor += gfx_char(fb, cursor, y, *s, scale, on);
    }
    return cursor - x;
}

int gfx_text_width(const char *s, int scale)
{
    if (s == NULL) {
        return 0;
    }
    return (int)strlen(s) * FONT_ADVANCE * (scale < 1 ? 1 : scale);
}

void gfx_text_window(uint8_t *fb, int x, int y, int window_w, const char *s,
                     int scale, int offset_px)
{
    if (s == NULL || window_w <= 0) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    const int advance = FONT_ADVANCE * scale;
    const int glyph_h = FONT_HEIGHT * scale;

    /* Skip whole glyphs that have scrolled off the left, then draw the rest
     * pixel-by-pixel so the leading glyph can be partially cut. */
    int index = offset_px / advance;
    int sub = offset_px % advance;
    const int len = (int)strlen(s);

    for (int cursor = -sub; cursor < window_w && index < len; index++, cursor += advance) {
        const char c = s[index];
        const uint8_t code = (c < 0x20 || c > 0x7E) ? (uint8_t)'?' : (uint8_t)c;
        const uint8_t *glyph = g_font5x7[code - 0x20];

        for (int col = 0; col < 5; col++) {
            for (int sx = 0; sx < scale; sx++) {
                const int px = cursor + col * scale + sx;
                if (px < 0 || px >= window_w) {
                    continue;   /* clipped by the window, not by the panel */
                }
                for (int row = 0; row < FONT_HEIGHT; row++) {
                    if (((glyph[col] >> row) & 1u) == 0) {
                        continue;
                    }
                    for (int sy = 0; sy < scale; sy++) {
                        gfx_pixel(fb, x + px, y + row * scale + sy, true);
                    }
                }
            }
        }
    }
    (void)glyph_h;
}

void gfx_battery(uint8_t *fb, int x, int y, uint8_t percent)
{
    /* 13x7 body with a 2x3 terminal on the right. */
    const int body_w = 13;
    const int body_h = 7;

    gfx_rect(fb, x, y, body_w, body_h, true);
    gfx_fill_rect(fb, x + body_w, y + 2, 2, 3, true);

    if (percent > 100) {
        percent = 100;
    }
    /* Two pixels of the outline plus a pixel of margin each side leaves nine
     * columns of fill, so each column is about 11%. */
    const int fill = (percent * 9 + 50) / 100;
    if (fill > 0) {
        gfx_fill_rect(fb, x + 2, y + 2, fill, body_h - 4, true);
    }
}
