/**
 * @file uart_handler.c
 * @brief UART G-code receiver for the single-board sample.
 */
#include "legacy_uart_route.h"
#include "common_def.h"
#include "legacy_uart_config.h"
#include "legacy_uart_gcode_parser.h"
#include "legacy_uart_gcode_processor.h"
#include "legacy_uart_motion_executor.h"
#include "laser_ctrl.h"
#include "pinctrl.h"
#include "preserve.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEGACY_UART_RX_LINE_MAX 128
#define LEGACY_UART_RX_BUF_SIZE 4096
#define LEGACY_UART_READ_TIMEOUT_MS 20
#define LEGACY_UART_GRBL_RESET_CHAR 0x18

static char g_rx_line[LEGACY_UART_RX_LINE_MAX];
static int g_rx_pos = 0;
static uint8_t g_uart_rx_buff[LEGACY_UART_RX_BUF_SIZE] = {0};
static uart_buffer_config_t g_uart_buffer_config = {
    .rx_buffer = g_uart_rx_buff,
    .rx_buffer_size = LEGACY_UART_RX_BUF_SIZE,
};
static volatile bool g_route_started = false;
#if LEGACY_UART_STATUS_PERIODIC
static unsigned long g_last_status_ms = 0;
#endif

static void wait_motion_idle(uint32_t timeout_ms);

static void uart_send_str(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    if (len > 0) {
        uapi_uart_write(LEGACY_UART_BUS, (const uint8_t *)str, len, 0);
    }
}

static void send_ok(void)
{
    uart_send_str("ok\r\n");
}

static void send_error(int code)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "error:%d\r\n", code);
    uart_send_str(buf);
}

static bool enqueue_motion_cmd(const legacy_uart_motion_cmd_t *cmd)
{
    if (!legacy_uart_motion_executor_enqueue(cmd)) {
        send_error(9);
        return false;
    }
    return true;
}

static void send_grbl_startup(const char *source)
{
    char buf[96];

    uart_send_str("\r\nWS63 Single Laser Marker V1.0\r\n");
    uart_send_str("Grbl 1.1f ['$' for help]\r\n");
    snprintf(buf, sizeof(buf), "[MSG:startup source=%s uptime=%lums reset=0x%04x count=%u]\r\n",
             source, (unsigned long)uapi_systick_get_ms(), (unsigned int)get_cpu_utils_reset_cause(),
             get_cpu_utils_reset_count());
    uart_send_str(buf);
}

static bool parsed_line_contains_gcode(const legacy_uart_gcode_line_t *gc, int expected_code)
{
    const char *p = gc->line;
    while ((p = strchr(p, 'G')) != NULL) {
        if ((p == gc->line || !((* (p - 1) >= 'A' && *(p - 1) <= 'Z') || (*(p - 1) >= 'a' && *(p - 1) <= 'z'))) &&
            atoi(p + 1) == expected_code) {
            return true;
        }
        p++;
    }
    return false;
}

static bool line_contains_mcode(const char *line, int expected_code)
{
    const char *p = line;
    while ((p = strchr(p, 'M')) != NULL) {
        if ((p == line || !isalpha((unsigned char)*(p - 1))) && atoi(p + 1) == expected_code) {
            return true;
        }
        p++;
    }
    return false;
}

static bool machine_is_idle(void)
{
    if (legacy_uart_motion_executor_is_busy()) {
        return false;
    }
    unsigned long last = legacy_uart_motion_executor_last_activity_ms();
    if (last == 0) {
        return true;
    }
    return ((unsigned long)uapi_systick_get_ms() - last) > LEGACY_UART_ACTIVITY_TIMEOUT_MS;
}

static void send_status_report(void)
{
    char buf[128];
    const char *state = machine_is_idle() ? "Idle" : "Run";
    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d|Ln:%lu>\r\n", state,
             legacy_uart_motion_executor_get_x(), legacy_uart_motion_executor_get_y(), (int)legacy_uart_gcode_processor_get_feed_rate(),
             (int)legacy_uart_gcode_processor_get_laser_power(), legacy_uart_gcode_processor_get_line_count());
    uart_send_str(buf);
}

static void send_periodic_status(void)
{
#if LEGACY_UART_STATUS_PERIODIC
    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - g_last_status_ms) >= LEGACY_UART_STATUS_INTERVAL_MS) {
        send_status_report();
        g_last_status_ms = now;
    }
#endif
}

static void send_wait_status(unsigned long *last_status_ms)
{
    if (last_status_ms == NULL) {
        return;
    }

    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - *last_status_ms) >= LEGACY_UART_STATUS_INTERVAL_MS) {
        send_status_report();
        *last_status_ms = now;
    }
}

static bool handle_dollar_command(const char *line)
{
    char buf[512];

    if (line[0] != '$') {
        return false;
    }

    if (strncmp(line, "$J=", 3) == 0) {
        legacy_uart_gcode_line_t gc;
        legacy_uart_gcode_init(&gc);
        for (int i = 3; line[i] != '\0' && gc.len < LEGACY_UART_GCODE_LINE_MAX - 1; i++) {
            legacy_uart_gcode_add_char(&gc, line[i]);
        }
        if (!legacy_uart_gcode_parse(&gc)) {
            send_error(2);
            return true;
        }

        bool relative = parsed_line_contains_gcode(&gc, 91);
        double target_x = legacy_uart_motion_executor_get_x();
        double target_y = legacy_uart_motion_executor_get_y();
        double feed_rate = legacy_uart_gcode_has_word(&gc, 'F') ? legacy_uart_gcode_get_value(&gc, 'F') : legacy_uart_gcode_processor_get_feed_rate();
        bool has_move = false;

        if (legacy_uart_gcode_has_word(&gc, 'X')) {
            double x = legacy_uart_gcode_get_value(&gc, 'X');
            target_x = relative ? (target_x + x) : x;
            has_move = true;
        }
        if (legacy_uart_gcode_has_word(&gc, 'Y')) {
            double y = legacy_uart_gcode_get_value(&gc, 'Y');
            target_y = relative ? (target_y + y) : y;
            has_move = true;
        }
        if (!has_move || feed_rate <= 0.0) {
            send_error(2);
            return true;
        }

        legacy_uart_motion_cmd_t cmd = {0};
        cmd.cmd = LEGACY_UART_CMD_G1_MOVE;
        cmd.target_x = (float)target_x;
        cmd.target_y = (float)target_y;
        cmd.feed_rate = (float)feed_rate;
        if (enqueue_motion_cmd(&cmd)) {
            send_ok();
        }
    } else if (strcmp(line, "$FRAME") == 0) {
        legacy_uart_motion_cmd_t cmd = {0};

        cmd.cmd = LEGACY_UART_CMD_G0_MOVE;
        cmd.target_x = (float)LEGACY_UART_GALVO_X_MIN_MM;
        cmd.target_y = (float)LEGACY_UART_GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }

        cmd.cmd = LEGACY_UART_CMD_G1_MOVE;
        cmd.feed_rate = (float)LEGACY_UART_FRAME_FEED_RATE;
        cmd.flags = LEGACY_UART_FLAG_LASER_ON;
        cmd.laser_pwr = LEGACY_UART_FRAME_LASER_POWER;
        cmd.target_x = (float)LEGACY_UART_GALVO_X_MAX_MM;
        cmd.target_y = (float)LEGACY_UART_GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)LEGACY_UART_GALVO_X_MAX_MM;
        cmd.target_y = (float)LEGACY_UART_GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)LEGACY_UART_GALVO_X_MIN_MM;
        cmd.target_y = (float)LEGACY_UART_GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)LEGACY_UART_GALVO_X_MIN_MM;
        cmd.target_y = (float)LEGACY_UART_GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }

        cmd.cmd = LEGACY_UART_CMD_LASER_OFF;
        cmd.flags = 0;
        cmd.laser_pwr = 0;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        send_ok();
    } else if (strcmp(line, "$I") == 0) {
        uart_send_str("[VER:1.1f.WS63_SINGLE:]\r\n[OPT:V,15,128]\r\nok\r\n");
    } else if (strcmp(line, "$G") == 0) {
        snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n",
                 legacy_uart_gcode_processor_is_absolute_mode() ? 90 : 91, legacy_uart_gcode_processor_laser_is_enabled() ? 3 : 5,
                 (int)legacy_uart_gcode_processor_get_feed_rate(), (int)legacy_uart_gcode_processor_get_laser_power());
        uart_send_str(buf);
    } else if (strcmp(line, "$D") == 0) {
        snprintf(buf, sizeof(buf),
                 "[MSG:motion busy=%d queue=%u abort=%d worker=%d enq=%lu exe=%lu x=%.3f y=%.3f laser=%d power=%u pwm=%d pclk=%lu period=%lu high=%lu low=%lu req=%u eff=%u late_max=%lu late_cnt=%lu slip=%lu seg=%lu short=%lu]\r\nok\r\n",
                 legacy_uart_motion_executor_is_busy() ? 1 : 0, (unsigned int)legacy_uart_motion_executor_queue_depth(),
                 legacy_uart_motion_executor_abort_requested() ? 1 : 0, legacy_uart_motion_executor_worker_started() ? 1 : 0,
                 legacy_uart_motion_executor_enqueued_count(), legacy_uart_motion_executor_executed_count(),
                 legacy_uart_motion_executor_get_x(), legacy_uart_motion_executor_get_y(), laser_is_enabled() ? 1 : 0,
                 (unsigned int)laser_get_power(), laser_pwm_is_opened() ? 1 : 0,
                 (unsigned long)laser_pwm_clock_hz(), (unsigned long)laser_pwm_period_ticks(),
                 (unsigned long)laser_pwm_high_ticks(), (unsigned long)laser_pwm_low_ticks(),
                 (unsigned int)laser_pwm_last_requested_power(), (unsigned int)laser_pwm_last_effective_power(),
                 legacy_uart_motion_executor_max_sample_late_us(), legacy_uart_motion_executor_late_sample_count(),
                 legacy_uart_motion_executor_missed_sample_count(), legacy_uart_motion_executor_motion_segment_count(),
                 legacy_uart_motion_executor_short_segment_count());
        uart_send_str(buf);
    } else if (strcmp(line, "$H") == 0) {
        wait_motion_idle(LEGACY_UART_MOTION_END_DRAIN_TIMEOUT_MS);
        legacy_uart_gcode_processor_set_origin();
        legacy_uart_motion_executor_set_origin();
        send_ok();
    } else if (strcmp(line, "$C") == 0) {
        uart_send_str("[GC:G0 G54 G17 G21 G90 G94 M5]\r\nok\r\n");
    } else if (strcmp(line, "$") == 0) {
        uart_send_str("$G - View gcode parser state\r\n");
        uart_send_str("$I - View build info\r\n");
        uart_send_str("$D - View motion debug state\r\n");
        uart_send_str("$X - Kill alarm lock\r\n");
        uart_send_str("$H - Set origin\r\n");
        send_ok();
    } else {
        send_ok();
    }

    return true;
}

static void handle_emergency_stop(void)
{
    legacy_uart_motion_cmd_t cmd;
    legacy_uart_gcode_processor_build_emergency_stop(&cmd);
    legacy_uart_motion_executor_execute(&cmd);
}

static void wait_motion_idle(uint32_t timeout_ms)
{
    unsigned long start = (unsigned long)uapi_systick_get_ms();
    unsigned long last_status_ms = 0;

    while (legacy_uart_motion_executor_is_busy()) {
        if (((unsigned long)uapi_systick_get_ms() - start) >= timeout_ms) {
            break;
        }
        send_wait_status(&last_status_ms);
        osal_msleep(1);
    }
}

static bool handle_realtime_char(uint8_t ch)
{
    switch (ch) {
        case '?':
            send_status_report();
            return true;
        case '!':
            return true;
        case '~':
            return true;
        case LEGACY_UART_GRBL_RESET_CHAR:
            g_rx_pos = 0;
            handle_emergency_stop();
            wait_motion_idle(100);
            legacy_uart_gcode_processor_init();
            legacy_uart_motion_executor_set_origin();
            send_grbl_startup("soft-reset");
            return true;
        default:
            return false;
    }
}

static void wait_motion_queue_watermark(void)
{
    while (legacy_uart_motion_executor_queue_depth() >= LEGACY_UART_MOTION_QUEUE_OK_WATERMARK) {
        send_periodic_status();
        osal_msleep(1);
    }
}

static void execute_gcode_line(const char *line, int len)
{
    legacy_uart_motion_cmd_t cmds[4];
    int cmd_count = 0;
    bool drain_before_ok = line_contains_mcode(line, 5);

    if (legacy_uart_gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        for (int i = 0; i < cmd_count; i++) {
            if (!enqueue_motion_cmd(&cmds[i])) {
                return;
            }
        }
        if (cmd_count > 0) {
            wait_motion_queue_watermark();
        }
        if (drain_before_ok) {
            wait_motion_idle(LEGACY_UART_MOTION_END_DRAIN_TIMEOUT_MS);
            laser_force_off();
        }
    }

    send_ok();
}

static void process_line(const char *line, int len)
{
    if (len == 0) {
        return;
    }

    if (strcmp(line, "?") == 0) {
        send_status_report();
        return;
    }

    if (strcmp(line, "!") == 0 || strcmp(line, "$STOP") == 0 || strcmp(line, "M112") == 0) {
        handle_emergency_stop();
        send_ok();
        return;
    }

    if (handle_dollar_command(line)) {
        return;
    }

    if (len >= LEGACY_UART_RX_LINE_MAX - 1) {
        send_error(1);
        return;
    }

    execute_gcode_line(line, len);
}

int legacy_uart_route_task_entry(void *arg)
{
    unused(arg);

    osal_msleep(500);
    send_grbl_startup("boot");

    uint8_t ch;
    while (1) {
        int32_t ret = uapi_uart_read(LEGACY_UART_BUS, &ch, 1, LEGACY_UART_READ_TIMEOUT_MS);
        if (ret <= 0) {
            send_periodic_status();
            osal_msleep(1);
            continue;
        }

        if (handle_realtime_char(ch)) {
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (g_rx_pos > 0) {
                g_rx_line[g_rx_pos] = '\0';
                process_line(g_rx_line, g_rx_pos);
                g_rx_pos = 0;
            }
        } else if (g_rx_pos < LEGACY_UART_RX_LINE_MAX - 1) {
            g_rx_line[g_rx_pos++] = (char)ch;
        }
    }

    return 0;
}

errcode_t legacy_uart_route_init(void)
{
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(LEGACY_UART_RX_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(LEGACY_UART_TX_PIN, LEGACY_UART_PIN_MODE);
    uapi_pin_set_mode(LEGACY_UART_RX_PIN, LEGACY_UART_PIN_MODE);

    uart_attr_t attr = {0};
    attr.baud_rate = LEGACY_UART_BAUD_RATE;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    uart_pin_config_t pin_cfg = {0};
    pin_cfg.tx_pin = LEGACY_UART_TX_PIN;
    pin_cfg.rx_pin = LEGACY_UART_RX_PIN;
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(LEGACY_UART_BUS);
    errcode_t ret = uapi_uart_init(LEGACY_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buffer_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LEGACY_UART] legacy_uart_route_init failed: 0x%x\r\n", ret);
        return ret;
    }

    return ERRCODE_SUCC;
}

errcode_t legacy_uart_route_start(void)
{
    if (g_route_started) {
        return ERRCODE_SUCC;
    }

    laser_force_off();
    legacy_uart_gcode_processor_init();
    legacy_uart_motion_executor_init();

    errcode_t ret = legacy_uart_route_init();
    if (ret != ERRCODE_SUCC) {
        laser_force_off();
        return ret;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(legacy_uart_route_task_entry, NULL, "legacy_uart", LEGACY_UART_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[LEGACY_UART] create UART RX task failed\r\n");
        laser_force_off();
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, LEGACY_UART_TASK_PRIO) != OSAL_SUCCESS) {
        osal_printk("[LEGACY_UART] set UART RX priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    ret = legacy_uart_motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LEGACY_UART] legacy_uart_motion_executor_start_task failed: 0x%x\r\n", ret);
        legacy_uart_route_force_stop();
        return ret;
    }

    g_route_started = true;
    return ERRCODE_SUCC;
}

bool legacy_uart_route_is_idle(void)
{
    if (!g_route_started) {
        return true;
    }
    return !legacy_uart_motion_executor_is_busy() && !legacy_uart_motion_executor_abort_requested();
}

void legacy_uart_route_force_stop(void)
{
    legacy_uart_motion_executor_request_abort();
    legacy_uart_motion_executor_flush();
    laser_force_off();
}
