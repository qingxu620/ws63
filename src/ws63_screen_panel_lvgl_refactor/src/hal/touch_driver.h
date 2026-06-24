/**
 * @file touch_driver.h
 * @brief Touch driver wrapper: FT6336 init + LVGL indev.
 */
#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "errcode.h"
#include "lvgl.h"

errcode_t touch_driver_init(void);
bool touch_is_pressed(void);
void touch_get_last_pos(int16_t *x, int16_t *y);

#endif
