/**
 * @file touch_driver.c
 * @brief Touch driver: FT6336 init + LVGL indev port.
 *
 * Reuses frozen ft6336_touch and screen_board directly.
 */
#include "touch_driver.h"
#include "ft6336_touch.h"
#include "screen_config.h"
#include "soc_osal.h"

#define TOUCH_SCROLL_LIMIT_PX       6
#define TOUCH_SCROLL_THROW_PERCENT 15
#define TOUCH_READ_FAIL_HOLD_COUNT  1

static bool g_pressed = false;
static int16_t g_last_x = 0;
static int16_t g_last_y = 0;
static uint8_t g_read_fail_count = 0;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    ft6336_touch_data_t touch;
    errcode_t ret = ft6336_read_touch(&touch);

    if (ret == ERRCODE_SUCC && touch.count > 0) {
        int32_t raw_x = touch.point[0].x;
        int32_t raw_y = touch.point[0].y;
        int32_t mapped_x = (SCREEN_LCD_NATIVE_HEIGHT - 1) - raw_y;
        int32_t mapped_y = raw_x;

        if (mapped_x < 0) mapped_x = 0;
        if (mapped_x >= SCREEN_LVGL_WIDTH) mapped_x = SCREEN_LVGL_WIDTH - 1;
        if (mapped_y < 0) mapped_y = 0;
        if (mapped_y >= SCREEN_LVGL_HEIGHT) mapped_y = SCREEN_LVGL_HEIGHT - 1;

        if (!g_pressed) {
            osal_printk("[TOUCH] raw=(%ld,%ld) -> lvgl=(%ld,%ld)\r\n",
                        (long)raw_x, (long)raw_y,
                        (long)mapped_x, (long)mapped_y);
        }
        g_pressed = true;
        g_read_fail_count = 0;
        g_last_x = (int16_t)mapped_x;
        g_last_y = (int16_t)mapped_y;
    } else if (ret == ERRCODE_SUCC) {
        g_pressed = false;
        g_read_fail_count = 0;
    } else if (ret != ERRCODE_SUCC) {
        if (g_read_fail_count < UINT8_MAX) {
            g_read_fail_count++;
        }
        if (g_read_fail_count > TOUCH_READ_FAIL_HOLD_COUNT) {
            g_pressed = false;
        }
    }

    data->point.x = g_last_x;
    data->point.y = g_last_y;
    data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

errcode_t touch_driver_init(void)
{
    errcode_t ret = ft6336_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[TOUCH] ft6336 init failed: 0x%x (continuing)\r\n", ret);
        return ret;
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_scroll_limit(indev, TOUCH_SCROLL_LIMIT_PX);
    lv_indev_set_scroll_throw(indev, TOUCH_SCROLL_THROW_PERCENT);
    osal_printk("[LVGL] touch indev registered\r\n");
    return ERRCODE_SUCC;
}

bool touch_is_pressed(void)       { return g_pressed; }
void touch_get_last_pos(int16_t *x, int16_t *y) { *x = g_last_x; *y = g_last_y; }
