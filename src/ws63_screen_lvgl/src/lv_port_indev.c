/**
 * @file lv_port_indev.c
 * @brief LVGL input device port for FT6336 capacitive touch.
 *
 * Reads touch via ft6336_read_touch() and reports to LVGL.
 */
#include "lv_port_indev.h"
#include "lvgl.h"
#include "ft6336_touch.h"
#include "lv_demo_panel.h"
#include "soc_osal.h"

static bool g_pressed = false;
static int16_t g_last_x = 0;
static int16_t g_last_y = 0;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    ft6336_touch_data_t touch;
    errcode_t ret = ft6336_read_touch(&touch);

    if (ret == ERRCODE_SUCC && touch.count > 0) {
        g_pressed = true;
        g_last_x = (int16_t)touch.point[0].x;
        g_last_y = (int16_t)touch.point[0].y;
    } else {
        g_pressed = false;
    }

    data->point.x = g_last_x;
    data->point.y = g_last_y;
    data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    lv_demo_panel_update_touch(g_pressed, g_last_x, g_last_y);
}

void lv_port_indev_init(void)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    osal_printk("[LVGL] touch indev registered\r\n");
}
