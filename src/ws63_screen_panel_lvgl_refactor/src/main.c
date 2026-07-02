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
#include "service/panel_transport_sle.h"
#include "service/panel_offline_job.h"
#include "service/panel_file_manager.h"
#include "ui/ui_manager.h"
#include <stdbool.h>

#define LVGL_TIMER_INDEX     1
#define LVGL_TIMER_PRIORITY  1
#define LVGL_TICK_MS         1
#define LVGL_TASK_STACK_SIZE 0x6000
#define LVGL_TASK_PRIORITY   25
#define LVGL_HANDLER_MS      10
#define PANEL_POWER_STABLE_MS 500
#define PANEL_FILE_BOOT_SCAN_DELAY_TICKS (200 / LVGL_HANDLER_MS)
#define PANEL_SLE_START_DELAY_TICKS (1000 / LVGL_HANDLER_MS)
#define PANEL_FAKE_PROGRESS_PERIOD_TICKS (100 / LVGL_HANDLER_MS)
#define PANEL_JOB_TIME_PERIOD_TICKS (1000 / LVGL_HANDLER_MS)

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

    spi_bus_park_pins_for_boot();
    osal_msleep(PANEL_POWER_STABLE_MS);

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
    ret = panel_offline_job_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] offline job task start failed: 0x%x\r\n", ret);
    }

    uint32_t tick_count = 0;
    bool file_scan_done = false;
    bool sle_started = false;

    /* Set initial scene */
    panel_model_set_scene(PANEL_SCENE_IDLE_NONE);

    while (1) {
        lv_timer_handler();
        ui_manager_update();

        /* LVGL drawing is synchronous in this port; always leave LCD deselected. */
        spi_bus_lcd_cs_high();

        osal_msleep(LVGL_HANDLER_MS);

        tick_count++;

        /* Mount SD and scan the root G-code files after the first visible UI frame. */
        if (!file_scan_done && tick_count >= PANEL_FILE_BOOT_SCAN_DELAY_TICKS) {
            osal_printk("[PANEL] boot SD scan trigger\r\n");
            panel_file_manager_refresh();
            file_scan_done = true;
        }

        /*
         * Start the SLE status mirror after the first UI frame has settled.
         * This keeps LCD/LVGL first-screen allocation and RF/SLE bring-up from
         * competing during boot, which is important now that the panel uses a
         * larger Chinese font and more UI pages.
         */
        if (!sle_started && tick_count >= PANEL_SLE_START_DELAY_TICKS) {
            ret = panel_transport_sle_start();
            if (ret != ERRCODE_SUCC) {
                osal_printk("[PANEL] SLE observer start failed: 0x%x (continuing fake UI)\r\n", ret);
            }
            sle_started = true;
        }

        /* Progress simulation for fake transfer/execution states */
        if (!g_model.live_status_active &&
            (g_model.state == SYS_STATE_RECEIVING ||
             g_model.state == SYS_STATE_SENDING ||
             g_model.state == SYS_STATE_RUNNING) &&
            (tick_count % PANEL_FAKE_PROGRESS_PERIOD_TICKS) == 0) {
            if (g_model.progress < 100) {
                panel_model_set_progress(g_model.progress + 1);
            } else if (g_model.state == SYS_STATE_RECEIVING) {
                panel_model_set_scene(PANEL_SCENE_HOST_READY);
            } else if (g_model.state == SYS_STATE_SENDING) {
                panel_model_set_scene(PANEL_SCENE_SCREEN_RUNNING);
            } else if (g_model.state == SYS_STATE_RUNNING) {
                panel_model_set_scene(g_model.owner == PANEL_OWNER_SCREEN ?
                                      PANEL_SCENE_SCREEN_DONE :
                                      PANEL_SCENE_HOST_DONE);
            }
        }

        /* Job time tick (every ~1s) */
        if ((tick_count % PANEL_JOB_TIME_PERIOD_TICKS) == 0) {
            panel_model_tick();
        }

    }

    return 0;
}

static void panel_entry(void)
{
    errcode_t ret = task_create("panel_task", panel_task, NULL,
                                LVGL_TASK_STACK_SIZE, LVGL_TASK_PRIORITY);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] create task failed\r\n");
        return;
    }
}

app_run(panel_entry);
