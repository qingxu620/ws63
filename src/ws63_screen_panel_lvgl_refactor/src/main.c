/**
 * @file main.c
 * @brief WS63 Screen Panel minimal main: hardware init + LVGL + demo state loop.
 */
#include "app_init.h"
#include "lvgl.h"
#include "soc_osal.h"
#include "timer.h"
#include "chip_core_irq.h"

#include "hal/spi_bus.h"
#include "hal/lcd_driver.h"
#include "hal/touch_driver.h"
#include "service/task_manager.h"
#include "service/panel_model.h"
#include "ui/ui_manager.h"

#define LVGL_TIMER_INDEX     1
#define LVGL_TIMER_PRIORITY  1
#define LVGL_TICK_MS         1
#define LVGL_TASK_STACK_SIZE 0x3000
#define LVGL_TASK_PRIORITY   25
#define LVGL_HANDLER_MS      5

/* Demo state cycle interval (ms) */
#define DEMO_STATE_INTERVAL_MS 4000

static timer_handle_t g_tick_timer = NULL;

static void lv_tick_timer_cb(uintptr_t data)
{
    (void)data;
    lv_tick_inc(LVGL_TICK_MS);
    uapi_timer_start(g_tick_timer, LVGL_TICK_MS * 1000, lv_tick_timer_cb, 0);
}

static int panel_task(void *arg)
{
    (void)arg;

    osal_printk("[PANEL] task begin\r\n");

    /* HAL init */
    errcode_t ret = spi_bus_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] spi_bus init failed: 0x%x\r\n", ret);
        return -1;
    }

    /* LVGL core init */
    lv_init();

    /* LCD + LVGL display */
    ret = lcd_driver_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] lcd init failed: 0x%x\r\n", ret);
        return -1;
    }

    /* Touch + LVGL indev */
    ret = touch_driver_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] touch init failed: 0x%x (continuing)\r\n", ret);
    }

    /* LVGL tick timer */
    uapi_timer_init();
    uapi_timer_adapter(LVGL_TIMER_INDEX, TIMER_1_IRQN, LVGL_TIMER_PRIORITY);
    uapi_timer_create(LVGL_TIMER_INDEX, &g_tick_timer);
    uapi_timer_start(g_tick_timer, LVGL_TICK_MS * 1000, lv_tick_timer_cb, 0);

    /* Model + UI */
    panel_model_init();
    ui_manager_init();

    osal_printk("[PANEL] init done, entering handler loop\r\n");

    /* Demo state machine */
    static const system_state_t demo_states[] = {
        SYS_STATE_NO_JOB,
        SYS_STATE_RECEIVING,
        SYS_STATE_READY,
        SYS_STATE_RUNNING,
        SYS_STATE_DONE,
        SYS_STATE_NO_JOB,
        SYS_STATE_ERROR,
        SYS_STATE_LINK_LOST,
    };
    uint32_t demo_idx = 0;
    uint32_t tick_count = 0;
    uint32_t demo_next_tick = DEMO_STATE_INTERVAL_MS / LVGL_HANDLER_MS;

    /* Set initial state */
    panel_model_set_state(demo_states[0]);

    while (1) {
        lv_timer_handler();
        ui_manager_update();
        osal_msleep(LVGL_HANDLER_MS);

        tick_count++;

        /* Progress simulation for RUNNING state */
        if (g_model.state == SYS_STATE_RUNNING && (tick_count % 20) == 0) {
            if (g_model.progress < 100) {
                panel_model_set_progress(g_model.progress + 1);
            }
        }

        /* Job time tick (every ~1s) */
        if ((tick_count % 200) == 0) {
            panel_model_tick();
        }

        /* Demo state cycling */
        if (tick_count >= demo_next_tick) {
            demo_next_tick = tick_count + DEMO_STATE_INTERVAL_MS / LVGL_HANDLER_MS;
            demo_idx = (demo_idx + 1) % (sizeof(demo_states) / sizeof(demo_states[0]));
            panel_model_set_state(demo_states[demo_idx]);
        }
    }

    return 0;
}

static void panel_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Panel UI (refactor)\r\n");
    osal_printk("  LVGL v9.3.0 + MSP3223 ILI9341 + FT6336\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = task_create("panel_task", panel_task, NULL,
                                LVGL_TASK_STACK_SIZE, LVGL_TASK_PRIORITY);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] create task failed\r\n");
        return;
    }
    osal_printk("[PANEL] task created\r\n");
}

app_run(panel_entry);
