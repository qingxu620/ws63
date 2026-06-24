/**
 * @file lcd_driver.c
 * @brief LCD driver: board init + ILI9341 init + LVGL display port.
 *
 * Reuses frozen screen_board and ili9341_lcd directly.
 */
#include "lcd_driver.h"
#include "screen_board.h"
#include "screen_config.h"
#include "ili9341_lcd.h"
#include "soc_osal.h"

#define DISP_BUF_LINES SCREEN_LVGL_DRAW_BUF_LINES
#define LCD_DEFAULT_BRIGHTNESS_PCT 80U

static uint16_t g_disp_buf[LCD_WIDTH * DISP_BUF_LINES];
static uint32_t g_flush_count = 0;

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t src_w = area->x2 - area->x1 + 1;
    int32_t x0 = area->x1 < 0 ? 0 : area->x1;
    int32_t y0 = area->y1 < 0 ? 0 : area->y1;
    int32_t x1 = area->x2 >= LCD_WIDTH ? LCD_WIDTH - 1 : area->x2;
    int32_t y1 = area->y2 >= LCD_HEIGHT ? LCD_HEIGHT - 1 : area->y2;

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
        osal_printk("[LCD] flush area=(%ld,%ld)-(%ld,%ld) w=%lu h=%lu bytes=%lu\r\n",
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
        osal_printk("[LCD] flush failed: 0x%x\r\n", ret);
    }

    lv_display_flush_ready(disp);
}

errcode_t lcd_driver_init(void)
{
    errcode_t ret = screen_board_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LCD] board init failed: 0x%x\r\n", ret);
        return ret;
    }

    ret = ili9341_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LCD] ili9341 init failed: 0x%x\r\n", ret);
        return ret;
    }
    osal_printk("[LCD] ok %ux%u\r\n", ili9341_width(), ili9341_height());

    ret = screen_lcd_bl_pwm_init(LCD_DEFAULT_BRIGHTNESS_PCT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LCD] brightness control unavailable, using full backlight\r\n");
    }

    /* LVGL display registration */
    lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, g_disp_buf, NULL,
                           sizeof(g_disp_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    osal_printk("[LVGL] display=%ux%u buffer=%ux%u bytes=%lu partial\r\n",
                LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH, DISP_BUF_LINES,
                (unsigned long)sizeof(g_disp_buf));
    return ERRCODE_SUCC;
}

errcode_t lcd_set_brightness(uint8_t brightness_pct)
{
    return screen_lcd_bl_set_brightness(brightness_pct);
}

uint16_t lcd_get_width(void)  { return LCD_WIDTH; }
uint16_t lcd_get_height(void) { return LCD_HEIGHT; }
