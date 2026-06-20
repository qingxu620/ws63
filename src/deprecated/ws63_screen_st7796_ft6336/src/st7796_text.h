/**
 * @file st7796_text.h
 * @brief ASCII text rendering for the ST7796 LCD.
 */
#ifndef WS63_ST7796_TEXT_H
#define WS63_ST7796_TEXT_H

#include <stdint.h>

/**
 * @brief Draw a single ASCII character at (x, y).
 * @param x  Top-left X (0-based).
 * @param y  Top-left Y (0-based).
 * @param ch ASCII character (32-126).
 * @param fg Foreground color (RGB565).
 * @param bg Background color (RGB565).
 * @return 0 on success, -1 if out of bounds or invalid char.
 */
int st7796_draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg);

/**
 * @brief Draw a null-terminated ASCII string at (x, y).
 * @param x   Top-left X (0-based).
 * @param y   Top-left Y (0-based).
 * @param str Null-terminated ASCII string (32-126).
 * @param fg  Foreground color (RGB565).
 * @param bg  Background color (RGB565).
 * @return Number of characters drawn, or -1 on error.
 */
int st7796_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);

#endif
