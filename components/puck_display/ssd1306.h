/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * Minimal SSD1306 panel driver plus the drawing primitives this project needs.
 * Component-private: the rest of the firmware talks to puck_display.h.
 *
 * Written rather than pulled from the component registry for the same reason
 * the Bluetooth components were: this repository builds with nothing but
 * ESP-IDF, on any machine, with no network at build time.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64
#define SSD1306_PAGES   (SSD1306_HEIGHT / 8)
#define SSD1306_FB_SIZE (SSD1306_WIDTH * SSD1306_PAGES)

/** Glyph width including the one-pixel gap that separates characters. */
#define FONT_ADVANCE 6
#define FONT_HEIGHT  7

/**
 * @brief Open the I2C bus and initialise the panel.
 *
 * @param sda_gpio  I2C data pin.
 * @param scl_gpio  I2C clock pin.
 * @param address   7-bit address, normally 0x3C or 0x3D.
 *
 * @return ESP_ERR_NOT_FOUND if nothing acknowledges at @p address.
 */
esp_err_t ssd1306_init(int sda_gpio, int scl_gpio, uint8_t address);

/** @brief Release the panel and the bus. */
void ssd1306_deinit(void);

/** @brief Push the whole framebuffer to the panel. */
esp_err_t ssd1306_flush(const uint8_t *fb);

/** @brief Turn the panel on or off without tearing down the bus. */
esp_err_t ssd1306_set_power(bool on);

/** @brief Set contrast, 0..255. */
esp_err_t ssd1306_set_contrast(uint8_t contrast);

/* ---- Drawing, all operating on a caller-owned framebuffer ---------------- */

void gfx_clear(uint8_t *fb);
void gfx_fill(uint8_t *fb);
void gfx_pixel(uint8_t *fb, int x, int y, bool on);
void gfx_hline(uint8_t *fb, int x, int y, int w, bool on);
void gfx_vline(uint8_t *fb, int x, int y, int h, bool on);
void gfx_rect(uint8_t *fb, int x, int y, int w, int h, bool on);
void gfx_fill_rect(uint8_t *fb, int x, int y, int w, int h, bool on);

/**
 * @brief Draw one character.
 *
 * @param scale  1 for 5x7, 2 for 10x14.
 * @return Pixels advanced.
 */
int gfx_char(uint8_t *fb, int x, int y, char c, int scale, bool on);

/**
 * @brief Draw a string, clipped at the right edge of the panel.
 *
 * @return Pixels advanced.
 */
int gfx_text(uint8_t *fb, int x, int y, const char *s, int scale, bool on);

/**
 * @brief Draw a string clipped to a window, with a horizontal offset.
 *
 * This is what makes marquee scrolling possible: the caller advances
 * @p offset_px and the text slides through the window without spilling.
 */
void gfx_text_window(uint8_t *fb, int x, int y, int window_w, const char *s,
                     int scale, int offset_px);

/** @brief Pixel width a string would occupy at @p scale. */
int gfx_text_width(const char *s, int scale);

/** @brief Battery outline with a fill proportional to @p percent. */
void gfx_battery(uint8_t *fb, int x, int y, uint8_t percent);

/** Width in pixels of the signal meter, so callers can lay out around it. */
#define SIGNAL_METER_WIDTH 11

/**
 * @brief Four ascending signal bars.
 *
 * @param bars   0..4 filled. Empty bars still show their base, so the meter
 *               reads as "weak" rather than as a rendering fault.
 * @param known  false draws all four bases with no fill: no reading yet.
 */
void gfx_signal_bars(uint8_t *fb, int x, int y, uint8_t bars, bool known);
