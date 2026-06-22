/**
 * @file ili9341_lcd.h
 * @brief ILI9341V SPI LCD driver for MSP3223 (240x320, IPS).
 */
#ifndef WS63_ILI9341_LCD_H
#define WS63_ILI9341_LCD_H

#include <stdint.h>
#include "errcode.h"

typedef enum {
    ILI9341_ROTATION_0 = 0,
    ILI9341_ROTATION_90,
    ILI9341_ROTATION_180,
    ILI9341_ROTATION_CCW_90,
    ILI9341_ROTATION_270 = ILI9341_ROTATION_CCW_90,
} ili9341_rotation_t;

errcode_t ili9341_init(void);
errcode_t ili9341_set_rotation(ili9341_rotation_t rotation);
uint16_t ili9341_width(void);
uint16_t ili9341_height(void);
errcode_t ili9341_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
errcode_t ili9341_write_pixels_raw(const uint8_t *pixels, uint32_t byte_len);
errcode_t ili9341_write_pixels_rgb565(const uint16_t *pixels, uint32_t count);
errcode_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
errcode_t ili9341_clear(uint16_t color);

#endif
