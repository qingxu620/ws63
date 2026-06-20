/**
 * @file lv_port_disp.c
 * @brief LVGL display port for ST7796 LCD (320x480, RGB565).
 *
 * LVGL v9 with LV_COLOR_16_SWAP=1:
 *   - Renders in native (little-endian) format
 *   - Swaps to big-endian at flush time (call_flush_cb in lv_refr.c)
 *   - flush_cb receives big-endian pixels (MSB first, matching ST7796)
 *
 * st7796_write_pixels_rgb565() does its own byte swap (native→big-endian),
 * which would double-swap LVGL output. We bypass it with a direct SPI write
 * that sends the already-big-endian pixels as-is.
 */
#include "lv_port_disp.h"
#include "lvgl.h"
#include "st7796_lcd.h"
#include "screen_board.h"
#include "screen_config.h"
#include "soc_osal.h"

#define DISP_HOR_RES  SCREEN_LCD_WIDTH   /* 320 */
#define DISP_VER_RES  SCREEN_LCD_HEIGHT  /* 480 */
#define DISP_BUF_LINES 48

static uint16_t g_disp_buf[DISP_HOR_RES * DISP_BUF_LINES];

/**
 * @brief Flush callback for LVGL.
 *
 * LVGL v9 with LV_COLOR_16_SWAP=1 already swaps pixels to big-endian
 * before calling flush_cb. We write them directly to SPI without the
 * extra byte swap that st7796_write_pixels_rgb565() performs.
 */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t x0 = area->x1;
    uint16_t y0 = area->y1;
    uint16_t x1 = area->x2;
    uint16_t y1 = area->y2;
    uint32_t count = (uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1);

    st7796_set_window(x0, y0, x1, y1);

    /* px_map is already big-endian (MSB first) from LVGL's swap.
     * Send raw bytes directly — no additional byte swap. */
    screen_lcd_spi_write(px_map, count * 2);

    lv_display_flush_ready(disp);
}

void lv_port_disp_init(void)
{
    lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, g_disp_buf, NULL,
                           sizeof(g_disp_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    osal_printk("[LVGL] display buffer size=%dx%d\r\n", DISP_HOR_RES, DISP_BUF_LINES);
    osal_printk("[LVGL] flush_cb registered\r\n");
}
