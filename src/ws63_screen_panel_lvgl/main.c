/**
 * @file main.c
 * @brief WS63 industrial panel UI: MSP3223 ILI9341 + FT6336 + LVGL v9.3.0.
 */
#include "app_init.h"
#include "screen_board.h"
#include "screen_config.h"
#include "ili9341_lcd.h"
#include "ft6336_touch.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "panel_ui.h"
#include "lvgl.h"
#include "soc_osal.h"
#include "timer.h"
#include "chip_core_irq.h"

#define LVGL_TIMER_INDEX     1
#define LVGL_TIMER_PRIORITY  1
#define LVGL_TICK_MS         1
#define LVGL_TASK_STACK_SIZE 0x3000
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

    osal_printk("[PANEL] init begin\r\n");

    errcode_t ret = screen_board_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] board init failed: 0x%x\r\n", ret);
        return -1;
    }

    ret = ili9341_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] ili9341 init failed: 0x%x\r\n", ret);
        return -1;
    }
    osal_printk("[PANEL] lcd ok %ux%u\r\n", ili9341_width(), ili9341_height());

    ret = ft6336_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] ft6336 init failed: 0x%x (continuing)\r\n", ret);
    }

    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();

    uapi_timer_init();
    uapi_timer_adapter(LVGL_TIMER_INDEX, TIMER_1_IRQN, LVGL_TIMER_PRIORITY);
    uapi_timer_create(LVGL_TIMER_INDEX, &g_tick_timer);
    uapi_timer_start(g_tick_timer, LVGL_TICK_MS * 1000, lv_tick_timer_cb, 0);

    panel_ui_create();

    /* Demo: cycle through states for testing */
    panel_ui_set_state(SYS_STATE_NO_JOB);

    osal_printk("[PANEL] init done, entering handler loop\r\n");

    while (1) {
        lv_timer_handler();
        osal_msleep(LVGL_HANDLER_MS);
    }

    return 0;
}

static void lvgl_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Panel UI\r\n");
    osal_printk("  LVGL v9.3.0 + MSP3223 ILI9341 + FT6336\r\n");
    osal_printk("========================================\r\n");

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(lvgl_task, NULL, "panel_task",
                                          LVGL_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[PANEL] create task failed\r\n");
        return;
    }
    osal_kthread_set_priority(task, LVGL_TASK_PRIORITY);
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[PANEL] task created\r\n");
}

app_run(lvgl_entry);
