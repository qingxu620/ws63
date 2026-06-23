/**
 * @file lv_demo_panel.h
 * @brief LVGL demo page: WS63 Laser Panel.
 */
#ifndef LV_DEMO_PANEL_H
#define LV_DEMO_PANEL_H

#include <stdbool.h>
#include <stdint.h>

void lv_demo_panel_create(void);
void lv_demo_panel_update_touch(bool pressed, int16_t x, int16_t y);

#endif
