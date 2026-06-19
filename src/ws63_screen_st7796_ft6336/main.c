/**
 * @file main.c
 * @brief WS63 screen module: ST7796 LCD + FT6336 touch self-test.
 */
#include "app_init.h"
#include "screen_board.h"
#include "screen_config.h"
#include "st7796_lcd.h"
#include "st7796_text.h"
#include "ft6336_touch.h"
#include "soc_osal.h"

#include <stdio.h>

#define TITLE_X     8
#define TITLE_Y     16
#define LINE1_X     8
#define LINE1_Y     48
#define LINE2_X     8
#define LINE2_Y     72
#define LINE3_X     8
#define LINE3_Y     96
#define LINE4_X     8
#define LINE4_Y     120
#define LINE5_X     8
#define LINE5_Y     144
#define TOUCH_X     8
#define TOUCH_Y     176

#define CROSS_HALF  5
#define CROSS_COLOR SCREEN_COLOR_GREEN
#define ERASE_COLOR SCREEN_COLOR_BLACK
#define TOUCH_POLL_MS  50

#if SCREEN_LCD_ONLY_COLOR_TEST

/* LCD-only color test: skip touch, loop color fills forever */
static int screen_task(void *arg)
{
    (void)arg;

    osal_printk("[SCREEN] task start (LCD-only color test)\r\n");
    osal_msleep(500);

    osal_printk("[SCREEN] board init begin\r\n");
    errcode_t ret = screen_board_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SCREEN] board init failed: 0x%x\r\n", ret);
        return -1;
    }
    osal_printk("[SCREEN] board init ok\r\n");

    osal_printk("[SCREEN] st7796 init begin\r\n");
    ret = st7796_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SCREEN] st7796 init failed: 0x%x\r\n", ret);
        return -1;
    }
    osal_printk("[SCREEN] st7796 init ok size=%u,%u\r\n", st7796_width(), st7796_height());

    osal_printk("[SCREEN] color test begin\r\n");

    while (1) {
        osal_printk("[SCREEN] fill RED\r\n");
        (void)st7796_clear(SCREEN_COLOR_RED);
        osal_msleep(1000);

        osal_printk("[SCREEN] fill GREEN\r\n");
        (void)st7796_clear(SCREEN_COLOR_GREEN);
        osal_msleep(1000);

        osal_printk("[SCREEN] fill BLUE\r\n");
        (void)st7796_clear(SCREEN_COLOR_BLUE);
        osal_msleep(1000);

        osal_printk("[SCREEN] fill WHITE\r\n");
        (void)st7796_clear(SCREEN_COLOR_WHITE);
        osal_msleep(1000);

        osal_printk("[SCREEN] fill BLACK\r\n");
        (void)st7796_clear(SCREEN_COLOR_BLACK);
        osal_msleep(1000);
    }

    return 0;
}

#else /* Normal mode with touch */

static int16_t g_prev_cx = -1;
static int16_t g_prev_cy = -1;
static uint8_t g_was_pressed = 0;

static void screen_map_touch_to_lcd(uint16_t raw_x, uint16_t raw_y,
                                    uint16_t *lcd_x, uint16_t *lcd_y)
{
    *lcd_x = raw_x;
    *lcd_y = raw_y;
}

static void draw_crosshair(uint16_t cx, uint16_t cy, uint16_t color)
{
    uint16_t w = st7796_width();
    uint16_t h = st7796_height();

    uint16_t hx = (cx >= CROSS_HALF) ? cx - CROSS_HALF : 0;
    uint16_t hw = CROSS_HALF * 2 + 1;
    if (hx + hw > w) {
        hw = w - hx;
    }
    (void)st7796_fill_rect(hx, cy, hw, 1, color);

    uint16_t vy = (cy >= CROSS_HALF) ? cy - CROSS_HALF : 0;
    uint16_t vh = CROSS_HALF * 2 + 1;
    if (vy + vh > h) {
        vh = h - vy;
    }
    (void)st7796_fill_rect(cx, vy, 1, vh, color);
}

static void erase_prev_crosshair(void)
{
    if (g_prev_cx >= 0 && g_prev_cy >= 0) {
        draw_crosshair((uint16_t)g_prev_cx, (uint16_t)g_prev_cy, ERASE_COLOR);
        g_prev_cx = -1;
        g_prev_cy = -1;
    }
}

static void draw_selftest_page(void)
{
    (void)st7796_clear(SCREEN_COLOR_BLACK);

    (void)st7796_draw_string(TITLE_X, TITLE_Y,
        "WS63 SCREEN SELF TEST", SCREEN_COLOR_WHITE, SCREEN_COLOR_BLACK);

    (void)st7796_draw_string(LINE1_X, LINE1_Y,
        "LCD: ST7796 OK", SCREEN_COLOR_GREEN, SCREEN_COLOR_BLACK);

    (void)st7796_draw_string(LINE2_X, LINE2_Y,
        "TOUCH: WAITING", SCREEN_COLOR_YELLOW, SCREEN_COLOR_BLACK);

    (void)st7796_draw_string(LINE3_X, LINE3_Y,
        "I2C1: OK", SCREEN_COLOR_GREEN, SCREEN_COLOR_BLACK);

    (void)st7796_draw_string(LINE4_X, LINE4_Y,
        "SD_CS: GPIO14 RESERVED", SCREEN_COLOR_BLUE, SCREEN_COLOR_BLACK);

    (void)st7796_draw_string(TOUCH_X, TOUCH_Y,
        "Touch: x=--- y=---", SCREEN_COLOR_WHITE, SCREEN_COLOR_BLACK);
}

static void update_touch_status(uint8_t status)
{
    switch (status) {
        case 0:
            (void)st7796_draw_string(LINE2_X, LINE2_Y,
                "TOUCH: WAITING ", SCREEN_COLOR_YELLOW, SCREEN_COLOR_BLACK);
            break;
        case 1:
            (void)st7796_draw_string(LINE2_X, LINE2_Y,
                "TOUCH: PRESSED ", SCREEN_COLOR_RED, SCREEN_COLOR_BLACK);
            break;
        case 2:
            (void)st7796_draw_string(LINE2_X, LINE2_Y,
                "TOUCH: I2C FAIL", SCREEN_COLOR_RED, SCREEN_COLOR_BLACK);
            break;
        default:
            break;
    }
}

static void update_touch_coord(uint16_t lx, uint16_t ly)
{
    char buf[32];
    (void)snprintf(buf, sizeof(buf), "Touch: x=%3u y=%3u", lx, ly);
    (void)st7796_draw_string(TOUCH_X, TOUCH_Y,
        buf, SCREEN_COLOR_WHITE, SCREEN_COLOR_BLACK);
}

static void clear_touch_coord(void)
{
    (void)st7796_draw_string(TOUCH_X, TOUCH_Y,
        "Touch: x=--- y=---", SCREEN_COLOR_WHITE, SCREEN_COLOR_BLACK);
}

static int screen_task(void *arg)
{
    (void)arg;

    osal_printk("[SCREEN] task start\r\n");
    osal_msleep(500);

    osal_printk("[SCREEN] board init begin\r\n");
    errcode_t ret = screen_board_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SCREEN] board init failed: 0x%x\r\n", ret);
        return -1;
    }
    osal_printk("[SCREEN] board init ok\r\n");

    osal_printk("[SCREEN] st7796 init begin\r\n");
    ret = st7796_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SCREEN] st7796 init failed: 0x%x\r\n", ret);
        return -1;
    }
    osal_printk("[SCREEN] st7796 init ok size=%u,%u\r\n", st7796_width(), st7796_height());

    osal_printk("[SCREEN] color test begin\r\n");
    (void)st7796_clear(SCREEN_COLOR_RED);
    osal_msleep(300);
    (void)st7796_clear(SCREEN_COLOR_GREEN);
    osal_msleep(300);
    (void)st7796_clear(SCREEN_COLOR_BLUE);
    osal_msleep(300);
    osal_printk("[SCREEN] color test done\r\n");

    osal_printk("[SCREEN] ft6336 init begin\r\n");
    uint8_t touch_ok = 0;
    ret = ft6336_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SCREEN] ft6336 init failed: 0x%x\r\n", ret);
    } else {
        osal_printk("[SCREEN] ft6336 init ok\r\n");
        touch_ok = 1;
    }

    draw_selftest_page();
    if (!touch_ok) {
        update_touch_status(2);
    }
    osal_printk("[SCREEN] self-test page drawn\r\n");

    if (!touch_ok) {
        while (1) {
            osal_msleep(1000);
        }
    }

    while (1) {
        ft6336_touch_data_t touch;
        ret = ft6336_read_touch(&touch);

        if (ret == ERRCODE_SUCC && touch.count > 0) {
            uint16_t raw_x = touch.point[0].x;
            uint16_t raw_y = touch.point[0].y;
            uint16_t lcd_x = 0;
            uint16_t lcd_y = 0;

            screen_map_touch_to_lcd(raw_x, raw_y, &lcd_x, &lcd_y);

            osal_printk("[SCREEN] touch raw=(%u,%u) lcd=(%u,%u)\r\n",
                        raw_x, raw_y, lcd_x, lcd_y);

            if (!g_was_pressed) {
                update_touch_status(1);
                g_was_pressed = 1;
            }
            update_touch_coord(lcd_x, lcd_y);

            erase_prev_crosshair();
            draw_crosshair(lcd_x, lcd_y, CROSS_COLOR);
            g_prev_cx = (int16_t)lcd_x;
            g_prev_cy = (int16_t)lcd_y;
        } else {
            if (g_was_pressed) {
                update_touch_status(0);
                clear_touch_coord();
                erase_prev_crosshair();
                g_was_pressed = 0;
            }
        }

        osal_msleep(TOUCH_POLL_MS);
    }

    return 0;
}

#endif /* SCREEN_LCD_ONLY_COLOR_TEST */

static void screen_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Screen Module\r\n");
    osal_printk("  ST7796 LCD + FT6336 Touch\r\n");
    osal_printk("========================================\r\n");

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(screen_task, NULL, "screen_task", 0x1000);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[SCREEN] create task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, 25) != OSAL_SUCCESS) {
        osal_printk("[SCREEN] set priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[SCREEN] task created\r\n");
}

app_run(screen_entry);
