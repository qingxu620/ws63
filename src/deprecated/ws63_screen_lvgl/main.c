/**
 * @file main.c
 * @brief WS63 LVGL minimal port: MSP3223 ILI9341 LCD + FT6336 touch + LVGL v9.3.0.
 *
 * Initialization order:
 *   1. Board init (GPIO, SPI, I2C)
 *   2. ILI9341 LCD init
 *   3. FT6336 touch init
 *   4. LVGL init
 *   5. Display buffer + flush_cb
 *   6. Touch indev + read_cb
 *   7. Tick timer (hardware timer, 1ms)
 *   8. Demo page: WS63 Laser Panel
 *   9. lv_timer_handler() loop
 */
#include "app_init.h"
#include "screen_board.h"
#include "screen_config.h"
#include "ili9341_lcd.h"
#include "ft6336_touch.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "lv_demo_panel.h"
#include "lvgl.h"
#include "soc_osal.h"
#include "timer.h"
#include "chip_core_irq.h"

#define LVGL_TIMER_INDEX     1
#define LVGL_TIMER_PRIORITY  1
#define LVGL_TICK_MS         1
#define LVGL_TASK_STACK_SIZE 0x2000
#define LVGL_TASK_PRIORITY   25
#define LVGL_HANDLER_MS      5

static timer_handle_t g_tick_timer = NULL;

static void lv_tick_timer_cb(uintptr_t data)
{
    (void)data;
    lv_tick_inc(LVGL_TICK_MS);
    uapi_timer_start(g_tick_timer, LVGL_TICK_MS * 1000, lv_tick_timer_cb, 0);
}

static int lvgl_task(void *arg)
{
    (void)arg;

    osal_printk("[LVGL] init begin\r\n");

    /* Board init */
    errcode_t ret = screen_board_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LVGL] board init failed: 0x%x\r\n", ret);
        return -1;
    }

    /* LCD init */
    ret = ili9341_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LVGL] ili9341 init failed: 0x%x\r\n", ret);
        return -1;
    }
    osal_printk("[LVGL] lcd ok size=%u,%u\r\n", ili9341_width(), ili9341_height());

    /* Color test: fill screen with primary colors */
    osal_printk("[LVGL] color test begin\r\n");
    ili9341_clear(SCREEN_COLOR_RED);
    osal_msleep(500);
    ili9341_clear(SCREEN_COLOR_GREEN);
    osal_msleep(500);
    ili9341_clear(SCREEN_COLOR_BLUE);
    osal_msleep(500);
    ili9341_clear(SCREEN_COLOR_BLACK);
    osal_printk("[LVGL] color test done\r\n");

    /* Touch init */
    ret = ft6336_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LVGL] ft6336 init failed: 0x%x (continuing without touch)\r\n", ret);
    }

    /* LVGL core init */
    lv_init();

    /* Display port */
    lv_port_disp_init();

    /* Input device port */
    lv_port_indev_init();

    /* Tick timer: hardware timer for lv_tick_inc */
    uapi_timer_init();
    uapi_timer_adapter(LVGL_TIMER_INDEX, TIMER_1_IRQN, LVGL_TIMER_PRIORITY);
    uapi_timer_create(LVGL_TIMER_INDEX, &g_tick_timer);
    uapi_timer_start(g_tick_timer, LVGL_TICK_MS * 1000, lv_tick_timer_cb, 0);
    osal_printk("[LVGL] timer handler task start\r\n");

    /* Demo page */
    lv_demo_panel_create();

    /* Force LVGL to refresh */
    osal_printk("[LVGL] force refresh\r\n");
    lv_refr_now(NULL);

    osal_printk("[LVGL] init done, entering handler loop\r\n");

    /* Main loop: call lv_timer_handler periodically */
    uint32_t loop_count = 0;
    while (1) {
        lv_timer_handler();
        loop_count++;
        if (loop_count <= 10 || (loop_count % 100) == 0) {
            osal_printk("[LVGL] handler loop #%lu\r\n", loop_count);
        }
        osal_msleep(LVGL_HANDLER_MS);
    }

    return 0;
}

static void lvgl_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 LVGL Minimal Port\r\n");
    osal_printk("  LVGL v9.3.0 + MSP3223 ILI9341 + FT6336\r\n");
    osal_printk("========================================\r\n");

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(lvgl_task, NULL, "lvgl_task", LVGL_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[LVGL] create task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, LVGL_TASK_PRIORITY) != OSAL_SUCCESS) {
        osal_printk("[LVGL] set priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[LVGL] task created\r\n");
}

app_run(lvgl_entry);
