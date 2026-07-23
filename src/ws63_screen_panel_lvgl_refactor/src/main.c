/**
 * @file main.c
 * @brief WS63 Screen Panel main: hardware init + LVGL + mirrored/standalone data flow.
 */
#include "app_init.h"
#include "lvgl.h"
#include "soc_osal.h"
#include "systick.h"
#include "timer.h"
#include "chip_core_irq.h"

#include "config.h"
#include "hal/spi_bus.h"
#include "hal/lcd_driver.h"
#include "hal/touch_driver.h"
#include "service/task_manager.h"
#include "service/panel_model.h"
#include "service/panel_transport_sle.h"
#include "service/panel_offline_job.h"
#include "service/panel_rx_commands.h"
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
#define PANEL_SLE_START_DELAY_TICKS (1000 / LVGL_HANDLER_MS)
#define PANEL_FILE_BOOT_SCAN_DELAY_TICKS (1500 / LVGL_HANDLER_MS)
#define PANEL_JOB_TIME_PERIOD_TICKS (1000 / LVGL_HANDLER_MS)
#define SCREEN_FIRMWARE_PACKAGE "ws63-liteos-app_screen_all.fwpkg"

static timer_handle_t g_tick_timer = NULL;
static volatile bool g_tick_timer_active;
static volatile errcode_t g_tick_timer_restart_error = ERRCODE_SUCC;

static void lv_tick_timer_cb(uintptr_t data)
{
    (void)data;
    lv_tick_inc(LVGL_TICK_MS);
    errcode_t ret = uapi_timer_start(g_tick_timer, LVGL_TICK_MS * 1000,
                                     lv_tick_timer_cb, 0);
    if (ret != ERRCODE_SUCC) {
        /* Do not print from the timer ISR; panel_task reports the failure. */
        g_tick_timer_restart_error = ret;
        g_tick_timer_active = false;
    }
}

static errcode_t lv_tick_timer_init(void)
{
    errcode_t ret = uapi_timer_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] LVGL Timer1 init failed: 0x%x; using systick fallback\r\n", ret);
        return ret;
    }

    ret = uapi_timer_adapter(LVGL_TIMER_INDEX, TIMER_1_IRQN, LVGL_TIMER_PRIORITY);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] LVGL Timer1 adapter failed: 0x%x; using systick fallback\r\n", ret);
        return ret;
    }

    ret = uapi_timer_create(LVGL_TIMER_INDEX, &g_tick_timer);
    if (ret != ERRCODE_SUCC) {
        g_tick_timer = NULL;
        osal_printk("[PANEL] LVGL Timer1 create failed: 0x%x; using systick fallback\r\n", ret);
        return ret;
    }

    g_tick_timer_restart_error = ERRCODE_SUCC;
    g_tick_timer_active = true;
    ret = uapi_timer_start(g_tick_timer, LVGL_TICK_MS * 1000, lv_tick_timer_cb, 0);
    if (ret != ERRCODE_SUCC) {
        g_tick_timer_active = false;
        osal_printk("[PANEL] LVGL Timer1 start failed: 0x%x; using systick fallback\r\n", ret);
        (void)uapi_timer_delete(g_tick_timer);
        g_tick_timer = NULL;
        return ret;
    }

    return ERRCODE_SUCC;
}

static int panel_task(void *arg)
{
    (void)arg;

    osal_printk("[FW_ID] board=SCREEN firmware=%s app=ws63_screen_panel_lvgl_refactor role=msp3223-panel-offline-sender lcd=ILI9341 touch=FT6336 sd=enabled\r\n",
                SCREEN_FIRMWARE_PACKAGE);

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

    /* LVGL keeps running from systick if Timer1 cannot be initialized or restarted. */
    (void)lv_tick_timer_init();

    /* Model + UI */
    panel_model_init();
    ui_manager_init();
    ret = panel_offline_job_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] offline job task start failed: 0x%x\r\n", ret);
    }
    ret = panel_rx_commands_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] RX command task start failed: 0x%x\r\n", ret);
    }

    uint32_t tick_count = 0;
    uint32_t fallback_tick_ms = (uint32_t)uapi_systick_get_ms();
    bool fallback_tick_active = !g_tick_timer_active;
    bool timer_restart_error_reported = false;
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

        uint32_t now_ms = (uint32_t)uapi_systick_get_ms();
        if (!g_tick_timer_active) {
            if (!fallback_tick_active) {
                /* Avoid double-counting the interval in which the HW timer failed. */
                fallback_tick_active = true;
            } else {
                uint32_t elapsed_ms = now_ms - fallback_tick_ms;
                if (elapsed_ms > 0U) {
                    lv_tick_inc(elapsed_ms);
                }
            }
            if (g_tick_timer_restart_error != ERRCODE_SUCC &&
                !timer_restart_error_reported) {
                osal_printk("[PANEL] LVGL Timer1 restart failed: 0x%x; switched to systick fallback\r\n",
                            g_tick_timer_restart_error);
                timer_restart_error_reported = true;
            }
        } else {
            fallback_tick_active = false;
        }
        fallback_tick_ms = now_ms;

        tick_count++;

        /* Queue SD work only after the first UI frame and SLE bring-up window. */
        if (!file_scan_done && tick_count >= PANEL_FILE_BOOT_SCAN_DELAY_TICKS) {
            osal_printk("[PANEL] boot SD scan trigger\r\n");
            ret = panel_file_manager_refresh();
            if (ret != ERRCODE_SUCC) {
                osal_printk("[PANEL] boot SD scan queue failed: 0x%x\r\n", ret);
            }
            file_scan_done = true;
        }

        /*
         * Start the SLE status mirror after the first UI frame has settled.
         * This keeps LCD/LVGL first-screen allocation and RF/SLE bring-up from
         * competing during boot, which is important now that the panel uses a
         * larger Chinese font and more UI pages.
         */
#if PANEL_ENABLE_SLE
        if (!sle_started && tick_count >= PANEL_SLE_START_DELAY_TICKS) {
            ret = panel_transport_sle_start();
            if (ret != ERRCODE_SUCC) {
                osal_printk("[PANEL] SLE observer start failed: 0x%x (continuing local UI)\r\n", ret);
            }
            sle_started = true;
        }
#else
        if (!sle_started && tick_count >= PANEL_SLE_START_DELAY_TICKS) {
            osal_printk("[PANEL] SLE disabled for isolation test; local UI only\r\n");
            sle_started = true;
        }
#endif

        /* The model derives elapsed time from systick, not loop iterations. */
        panel_model_tick();

    }

    return 0;
}

static void panel_entry(void)
{
    osal_printk("[FW_ID] board=SCREEN firmware=%s app=ws63_screen_panel_lvgl_refactor role=msp3223-panel-offline-sender\r\n",
                SCREEN_FIRMWARE_PACKAGE);

    errcode_t ret = task_create("panel_task", panel_task, NULL,
                                LVGL_TASK_STACK_SIZE, LVGL_TASK_PRIORITY);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL] create task failed\r\n");
        return;
    }
}

app_run(panel_entry);
