/**
 * @file st7796_lcd.h
 * @brief ST7796S SPI LCD driver.
 */
#ifndef WS63_ST7796_LCD_H
#define WS63_ST7796_LCD_H

#include <stdint.h>
#include "errcode.h"

typedef enum {
    ST7796_ROTATION_0 = 0,
    ST7796_ROTATION_90,
    ST7796_ROTATION_180,
    ST7796_ROTATION_270,
} st7796_rotation_t;

errcode_t st7796_init(void);
errcode_t st7796_set_rotation(st7796_rotation_t rotation);
uint16_t st7796_width(void);
uint16_t st7796_height(void);
errcode_t st7796_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
errcode_t st7796_write_pixels_rgb565(const uint16_t *pixels, uint32_t count);
errcode_t st7796_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
errcode_t st7796_clear(uint16_t color);

#endif
