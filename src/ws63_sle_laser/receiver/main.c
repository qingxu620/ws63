/**
 * @file main.c
 * @brief Receiver entry - receives G-code via SLE and executes laser marking.
 *
 * This board is essentially ws63_laser_single with SLE input instead of UART.
 * The SLE receiver module feeds G-code lines into the same processing pipeline.
 * Response (ok/error/status) is sent back via SLE.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "dac8562.h"
#include "gcode_parser.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "sle_receiver.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart.h"
#include "pinctrl.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* G-code processing - same as ws63_laser_single */
#define RX_LINE_MAX 128
#define GRBL_RESET_CHAR 0x18

static char g_rx_line[RX_LINE_MAX];
static int g_rx_pos = 0;

/* Forward declarations */
static void process_line(const char *line, int len);
static void send_ok(void);
static void send_error(int code);
static void send_response(const char *str);

/* Send response via both UART (for debug) and SLE (to transmitter) */
static void send_response(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    if (len == 0) return;

    /* Send via UART for local debug */
    uapi_uart_write(LASER_UART_BUS, (const uint8_t *)str, len, 0);

    /* Send via SLE back to transmitter */
    if (sle_receiver_is_connected()) {
        sle_receiver_send_response(str, (uint16_t)len);
    }
}

/* SLE G-code callback - called when G-code received from SLE */
void sle_gcode_line_received(const char *line, uint16_t len)
{
    if (line == NULL || len == 0) return;
    osal_printk("[rx] processing: %s\r\n", line);
    process_line(line, (int)len);
}

static void send_ok(void)
{
    send_response("ok\r\n");
}

static void send_error(int code)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "error:%d\r\n", code);
    send_response(buf);
}

/* G-code processing - copied from ws63_laser_single */
static bool enqueue_motion_cmd(const motion_cmd_t *cmd)
{
    if (!motion_executor_enqueue(cmd)) {
        send_error(9);
        return false;
    }
    return true;
}

static void send_grbl_startup(const char *source)
{
    char buf[96];
    send_response("\r\nWS63 SLE Laser Marker V1.0\r\n");
    send_response("Grbl 1.1f ['$' for help]\r\n");
    snprintf(buf, sizeof(buf), "[MSG:startup source=%s uptime=%lums]\r\n",
             source, (unsigned long)uapi_systick_get_ms());
    send_response(buf);
}

static bool machine_is_idle(void)
{
    if (motion_executor_is_busy()) return false;
    unsigned long last = motion_executor_last_activity_ms();
    if (last == 0) return true;
    return ((unsigned long)uapi_systick_get_ms() - last) > ACTIVITY_TIMEOUT_MS;
}

static void send_status_report(void)
{
    char buf[128];
    const char *state = machine_is_idle() ? "Idle" : "Run";
    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d>\r\n", state,
             motion_executor_get_x(), motion_executor_get_y(),
             (int)gcode_processor_get_feed_rate(),
             (int)gcode_processor_get_laser_power());
    send_response(buf);
}

static bool handle_dollar_command(const char *line)
{
    if (line[0] != '$') return false;

    if (strcmp(line, "$I") == 0) {
        send_response("[VER:1.1f.WS63_SLE:]\r\n[OPT:V,15,128]\r\nok\r\n");
    } else if (strcmp(line, "$G") == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n",
                 gcode_processor_is_absolute_mode() ? 90 : 91,
                 gcode_processor_laser_is_enabled() ? 3 : 5,
                 (int)gcode_processor_get_feed_rate(),
                 (int)gcode_processor_get_laser_power());
        send_response(buf);
    } else if (strcmp(line, "$H") == 0) {
        gcode_processor_set_origin();
        motion_executor_set_origin();
        send_ok();
    } else if (strcmp(line, "$") == 0) {
        send_response("$G - View gcode parser state\r\n");
        send_response("$I - View build info\r\n");
        send_response("$X - Kill alarm lock\r\n");
        send_response("$H - Set origin\r\n");
        send_ok();
    } else {
        send_ok();
    }
    return true;
}

static void handle_emergency_stop(void)
{
    motion_cmd_t cmd;
    gcode_processor_build_emergency_stop(&cmd);
    motion_executor_execute(&cmd);
}

static void execute_gcode_line(const char *line, int len)
{
    motion_cmd_t cmds[4];
    int cmd_count = 0;
    bool drain_before_ok = false;

    /* Check for M5 (laser off) */
    const char *p = line;
    while ((p = strchr(p, 'M')) != NULL) {
        if (atoi(p + 1) == 5) {
            drain_before_ok = true;
            break;
        }
        p++;
    }

    if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        for (int i = 0; i < cmd_count; i++) {
            if (!enqueue_motion_cmd(&cmds[i])) return;
        }
        if (drain_before_ok) {
            /* Wait for motion to complete before turning off laser */
            osal_msleep(100);
            laser_force_off();
        }
    }
    send_ok();
}

static void process_line(const char *line, int len)
{
    if (len == 0) return;

    if (strcmp(line, "?") == 0) {
        send_status_report();
        return;
    }

    if (strcmp(line, "!") == 0 || strcmp(line, "$STOP") == 0 || strcmp(line, "M112") == 0) {
        handle_emergency_stop();
        send_ok();
        return;
    }

    /* Handle Ctrl-X (soft reset) */
    if (len == 1 && line[0] == GRBL_RESET_CHAR) {
        handle_emergency_stop();
        gcode_processor_init();
        motion_executor_set_origin();
        send_grbl_startup("ctrl-x");
        return;
    }

    if (handle_dollar_command(line)) return;

    if (len >= RX_LINE_MAX - 1) {
        send_error(1);
        return;
    }

    execute_gcode_line(line, len);
}

/* SLE init task */
static int sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    errcode_t ret = sle_receiver_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] SLE init failed: 0x%x\r\n", ret);
        return -1;
    }
    return 0;
}

static void sle_laser_receiver_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 SLE Laser Receiver\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] dac init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] laser init failed: 0x%x\r\n", ret);
        return;
    }

    gcode_processor_init();
    motion_executor_init();

    osal_kthread_lock();

    /* SLE task */
    osal_task *task = osal_kthread_create(sle_init_task, NULL, "sle_init", 0x2000);
    if (task != NULL) {
        osal_kthread_set_priority(task, 26);
        osal_kfree(task);
    }

    /* Motion executor task */
    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] motion task failed: 0x%x\r\n", ret);
    }

    osal_kthread_unlock();

    send_grbl_startup("boot");
    osal_printk("[rx] ready, waiting for SLE connection...\r\n");
}

app_run(sle_laser_receiver_entry);
