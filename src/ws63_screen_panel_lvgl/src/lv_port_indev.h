#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#include <stdbool.h>
#include <stdint.h>

void lv_port_indev_init(void);

extern void (*g_panel_touch_cb)(bool pressed, int16_t x, int16_t y);

#endif
