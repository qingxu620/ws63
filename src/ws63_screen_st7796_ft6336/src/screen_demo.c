/**
 * @file screen_demo.c
 * @brief Minimal bring-up demo for the ST7796 + FT6336 screen board.
 */
#include "screen_board.h"
#include "screen_config.h"
#include "st7796_lcd.h"
#include "ft6336_touch.h"

#include "soc_osal.h"

void screen_demo_run(void)
{
    errcode_t ret = screen_board_init();
    osal_printk("[screen] board init ret=0x%x\r\n", ret);
    if (ret != ERRCODE_SUCC) {
        return;
    }

    ret = st7796_init();
    osal_printk("[screen] st7796 init ret=0x%x size=%u,%u\r\n", ret, st7796_width(), st7796_height());
    if (ret != ERRCODE_SUCC) {
        return;
    }

    (void)st7796_clear(SCREEN_COLOR_RED);
    screen_board_delay_ms(500);
    (void)st7796_clear(SCREEN_COLOR_GREEN);
    screen_board_delay_ms(500);
    (void)st7796_clear(SCREEN_COLOR_BLUE);
    screen_board_delay_ms(500);
    (void)st7796_clear(SCREEN_COLOR_BLACK);

    ret = ft6336_init();
    osal_printk("[screen] ft6336 init ret=0x%x\r\n", ret);

    while (1) {
        ft6336_touch_data_t touch;
        ret = ft6336_read_touch(&touch);
        if (ret == ERRCODE_SUCC && touch.count > 0) {
            osal_printk("[screen] touch n=%u x=%u y=%u e=%u id=%u\r\n",
                        touch.count,
                        touch.point[0].x,
                        touch.point[0].y,
                        touch.point[0].event,
                        touch.point[0].id);
        }
        osal_msleep(50);
    }
}
