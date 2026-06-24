/**
 * @file lcd_driver.h
 * @brief LCD driver wrapper: board init, ILI9341 init, LVGL flush.
 */
#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdint.h>
#include "errcode.h"
#include "lvgl.h"

#define LCD_WIDTH   320
#define LCD_HEIGHT  240

errcode_t lcd_driver_init(void);
errcode_t lcd_set_brightness(uint8_t brightness_pct);
uint16_t lcd_get_width(void);
uint16_t lcd_get_height(void);
void lcd_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);

#endif
