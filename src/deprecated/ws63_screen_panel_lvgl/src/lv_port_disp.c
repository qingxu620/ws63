/**
 * @file lv_port_disp.c
 * @brief LVGL display port for MSP3223 ILI9341 LCD (320x240 landscape, RGB565).
 */
#include "lv_port_disp.h"
#include "lvgl.h"
#include "ili9341_lcd.h"
#include "screen_config.h"
#include "soc_osal.h"

#define DISP_HOR_RES   SCREEN_LVGL_WIDTH
#define DISP_VER_RES   SCREEN_LVGL_HEIGHT
#define DISP_BUF_LINES SCREEN_LVGL_DRAW_BUF_LINES

static uint16_t g_disp_buf[DISP_HOR_RES * DISP_BUF_LINES];
static uint32_t g_flush_count = 0;

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t src_w = area->x2 - area->x1 + 1;
    int32_t x0 = area->x1 < 0 ? 0 : area->x1;
    int32_t y0 = area->y1 < 0 ? 0 : area->y1;
    int32_t x1 = area->x2 >= DISP_HOR_RES ? DISP_HOR_RES - 1 : area->x2;
    int32_t y1 = area->y2 >= DISP_VER_RES ? DISP_VER_RES - 1 : area->y2;

    if (x0 > x1 || y0 > y1) {
        lv_display_flush_ready(disp);
        return;
    }

    uint32_t w = (uint32_t)(x1 - x0 + 1);
    uint32_t h = (uint32_t)(y1 - y0 + 1);
    uint32_t byte_len = w * h * 2U;
    uint32_t src_x_offset = (uint32_t)(x0 - area->x1);
    uint32_t src_y_offset = (uint32_t)(y0 - area->y1);
    const uint8_t *src = px_map + ((src_y_offset * (uint32_t)src_w + src_x_offset) * 2U);

    g_flush_count++;
    if (g_flush_count <= 3) {
        osal_printk("[LVGL] flush area=(%ld,%ld)-(%ld,%ld) w=%lu h=%lu bytes=%lu\r\n",
                    (long)x0, (long)y0, (long)x1, (long)y1,
                    (unsigned long)w, (unsigned long)h, (unsigned long)byte_len);
    }

    errcode_t ret = ili9341_set_window((uint16_t)x0, (uint16_t)y0,
                                       (uint16_t)x1, (uint16_t)y1);
    if (ret == ERRCODE_SUCC) {
        if (w == (uint32_t)src_w) {
            ret = ili9341_write_pixels_rgb565((const uint16_t *)src, byte_len / 2U);
        } else {
            for (uint32_t row = 0; row < h && ret == ERRCODE_SUCC; row++) {
                ret = ili9341_write_pixels_rgb565(
                    (const uint16_t *)(src + row * (uint32_t)src_w * 2U), w);
            }
        }
    }
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LVGL] flush failed: 0x%x\r\n", ret);
    }

    lv_display_flush_ready(disp);
}

void lv_port_disp_init(void)
{
    lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, g_disp_buf, NULL,
                           sizeof(g_disp_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    osal_printk("[LVGL] display=%ux%u buffer=%ux%u bytes=%lu partial\r\n",
                DISP_HOR_RES, DISP_VER_RES, DISP_HOR_RES, DISP_BUF_LINES,
                (unsigned long)sizeof(g_disp_buf));
    osal_printk("[LVGL] flush_cb registered\r\n");
}
