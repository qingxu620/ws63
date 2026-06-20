/**
 * @file ft6336_touch.h
 * @brief FT6336U capacitive touch driver.
 */
#ifndef WS63_FT6336_TOUCH_H
#define WS63_FT6336_TOUCH_H

#include <stdbool.h>
#include <stdint.h>
#include "errcode.h"

#define FT6336_MAX_POINTS 2

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t event;
    uint8_t id;
} ft6336_point_t;

typedef struct {
    uint8_t count;
    ft6336_point_t point[FT6336_MAX_POINTS];
} ft6336_touch_data_t;

errcode_t ft6336_init(void);
errcode_t ft6336_read_ids(uint8_t *vendor, uint8_t *cipher_mid, uint8_t *cipher_low, uint8_t *cipher_high);
errcode_t ft6336_read_touch(ft6336_touch_data_t *touch);

#endif
