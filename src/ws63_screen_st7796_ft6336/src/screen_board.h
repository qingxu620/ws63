/**
 * @file screen_board.h
 * @brief WS63 hardware adapter for the ST7796 + FT6336 screen board.
 */
#ifndef WS63_SCREEN_BOARD_H
#define WS63_SCREEN_BOARD_H

#include <stdbool.h>
#include <stdint.h>
#include "errcode.h"

errcode_t screen_board_init(void);
void screen_board_delay_ms(uint32_t ms);
void screen_board_delay_us(uint32_t us);

void screen_lcd_cs(bool level);
void screen_lcd_dc(bool data_mode);
void screen_lcd_rst(bool level);
void screen_lcd_bl(bool level);
errcode_t screen_lcd_spi_write(const uint8_t *data, uint32_t len);

void screen_touch_rst(bool level);
bool screen_touch_int_level(void);
errcode_t screen_touch_i2c_write(uint8_t reg, const uint8_t *data, uint32_t len);
errcode_t screen_touch_i2c_read(uint8_t reg, uint8_t *data, uint32_t len);

#endif
