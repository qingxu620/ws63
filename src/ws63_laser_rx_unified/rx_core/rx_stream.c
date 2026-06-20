/**
 * @file rx_stream.c
 * @brief Shared byte-stream command parser for unified RX transports.
 */
#include "rx_stream.h"
#include "common_def.h"
#include "config.h"
#include "gcode_parser.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "rx_mode.h"
#include "safety.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart_transport.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_LINE_MAX 128
#define GRBL_RESET_CHAR 0x18

static char g_rx_line[RX_LINE_MAX];
static int g_rx_pos = 0;
#if RX_STREAM_STATUS_PERIODIC
static unsigned long g_last_status_ms = 0;
#endif

static void wait_motion_idle(rx_stream_src_t src, uint32_t timeout_ms);

static void stream_send_str(rx_stream_src_t src, const char *str)
{
    if (str == NULL) {
        return;
    }

    switch (src) {
        case RX_SRC_UART:
        default:
            uart_transport_write_str(str);
            break;
    }
}

static void send_ok(rx_stream_src_t src)
{
    stream_send_str(src, "ok\r\n");
}

static void send_error(rx_stream_src_t src, int code)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "error:%d\r\n", code);
    stream_send_str(src, buf);
}

static bool enqueue_motion_cmd(rx_stream_src_t src, const motion_cmd_t *cmd)
{
    if (!motion_executor_enqueue(cmd)) {
        send_error(src, 9);
        return false;
    }
    return true;
}

static void send_grbl_startup(rx_stream_src_t src, const char *source)
{
    char buf[96];

    stream_send_str(src, "\r\nWS63 Laser RX Unified\r\n");
    stream_send_str(src, "Grbl 1.1f ['$' for help]\r\n");
    snprintf(buf, sizeof(buf), "[MSG:startup source=%s uptime=%lums]\r\n",
             source, (unsigned long)uapi_systick_get_ms());
    stream_send_str(src, buf);
}

static bool parsed_line_contains_gcode(const gcode_line_t *gc, int expected_code)
{
    const char *p = gc->line;
    while ((p = strchr(p, 'G')) != NULL) {
        if ((p == gc->line || !(((*(p - 1) >= 'A') && (*(p - 1) <= 'Z')) ||
                                ((*(p - 1) >= 'a') && (*(p - 1) <= 'z')))) &&
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
    if (motion_executor_is_busy()) {
        return false;
    }
    unsigned long last = motion_executor_last_activity_ms();
    if (last == 0) {
        return true;
    }
    return ((unsigned long)uapi_systick_get_ms() - last) > ACTIVITY_TIMEOUT_MS;
}

static void send_status_report(rx_stream_src_t src)
{
    char buf[128];
    const char *state = machine_is_idle() ? "Idle" : "Run";

    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d|Ln:%lu>\r\n", state,
             motion_executor_get_x(), motion_executor_get_y(), (int)gcode_processor_get_feed_rate(),
             (int)gcode_processor_get_laser_power(), gcode_processor_get_line_count());
    stream_send_str(src, buf);
}

static void send_periodic_status(rx_stream_src_t src)
{
#if RX_STREAM_STATUS_PERIODIC
    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - g_last_status_ms) >= STATUS_INTERVAL_MS) {
        send_status_report(src);
        g_last_status_ms = now;
    }
#else
    unused(src);
#endif
}

static void send_wait_status(rx_stream_src_t src, unsigned long *last_status_ms)
{
    if (last_status_ms == NULL) {
        return;
    }

    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - *last_status_ms) >= STATUS_INTERVAL_MS) {
        send_status_report(src);
        *last_status_ms = now;
    }
}

static bool handle_dollar_command(rx_stream_src_t src, const char *line)
{
    char buf[512];

    if (line[0] != '$') {
        return false;
    }

    if (strncmp(line, "$J=", 3) == 0) {
        gcode_line_t gc;
        gcode_init(&gc);
        for (int i = 3; line[i] != '\0' && gc.len < GCODE_LINE_MAX - 1; i++) {
            gcode_add_char(&gc, line[i]);
        }
        if (!gcode_parse(&gc)) {
            send_error(src, 2);
            return true;
        }

        bool relative = parsed_line_contains_gcode(&gc, 91);
        double target_x = motion_executor_get_x();
        double target_y = motion_executor_get_y();
        double feed_rate = gcode_has_word(&gc, 'F') ? gcode_get_value(&gc, 'F') : gcode_processor_get_feed_rate();
        bool has_move = false;

        if (gcode_has_word(&gc, 'X')) {
            double x = gcode_get_value(&gc, 'X');
            target_x = relative ? (target_x + x) : x;
            has_move = true;
        }
        if (gcode_has_word(&gc, 'Y')) {
            double y = gcode_get_value(&gc, 'Y');
            target_y = relative ? (target_y + y) : y;
            has_move = true;
        }
        if (!has_move || feed_rate <= 0.0) {
            send_error(src, 2);
            return true;
        }

        motion_cmd_t cmd = {0};
        cmd.cmd = CMD_G1_MOVE;
        cmd.target_x = (float)target_x;
        cmd.target_y = (float)target_y;
        cmd.feed_rate = (float)feed_rate;
        if (enqueue_motion_cmd(src, &cmd)) {
            send_ok(src);
        }
    } else if (strcmp(line, "$FRAME") == 0) {
        motion_cmd_t cmd = {0};

        cmd.cmd = CMD_G0_MOVE;
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(src, &cmd)) {
            return true;
        }

        cmd.cmd = CMD_G1_MOVE;
        cmd.feed_rate = (float)FRAME_FEED_RATE;
        cmd.flags = FLAG_LASER_ON;
        cmd.laser_pwr = FRAME_LASER_POWER;
        cmd.target_x = (float)GALVO_X_MAX_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(src, &cmd)) {
            return true;
        }
        cmd.target_x = (float)GALVO_X_MAX_MM;
        cmd.target_y = (float)GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(src, &cmd)) {
            return true;
        }
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(src, &cmd)) {
            return true;
        }
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(src, &cmd)) {
            return true;
        }

        cmd.cmd = CMD_LASER_OFF;
        cmd.flags = 0;
        cmd.laser_pwr = 0;
        if (!enqueue_motion_cmd(src, &cmd)) {
            return true;
        }
        send_ok(src);
    } else if (strcmp(line, "$I") == 0) {
        stream_send_str(src, "[VER:1.1f.WS63_RX_UNIFIED:]\r\n[OPT:V,15,128]\r\nok\r\n");
    } else if (strcmp(line, "$G") == 0) {
        snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n",
                 gcode_processor_is_absolute_mode() ? 90 : 91, gcode_processor_laser_is_enabled() ? 3 : 5,
                 (int)gcode_processor_get_feed_rate(), (int)gcode_processor_get_laser_power());
        stream_send_str(src, buf);
    } else if (strcmp(line, "$D") == 0) {
        snprintf(buf, sizeof(buf),
                 "[MSG:motion busy=%d queue=%u abort=%d worker=%d enq=%lu exe=%lu x=%.3f y=%.3f laser=%d power=%u pwm=%d pclk=%lu period=%lu high=%lu low=%lu req=%u eff=%u late_max=%lu late_cnt=%lu slip=%lu seg=%lu short=%lu]\r\nok\r\n",
                 motion_executor_is_busy() ? 1 : 0, (unsigned int)motion_executor_queue_depth(),
                 motion_executor_abort_requested() ? 1 : 0, motion_executor_worker_started() ? 1 : 0,
                 motion_executor_enqueued_count(), motion_executor_executed_count(),
                 motion_executor_get_x(), motion_executor_get_y(), laser_is_enabled() ? 1 : 0,
                 (unsigned int)laser_get_power(), laser_pwm_is_opened() ? 1 : 0,
                 (unsigned long)laser_pwm_clock_hz(), (unsigned long)laser_pwm_period_ticks(),
                 (unsigned long)laser_pwm_high_ticks(), (unsigned long)laser_pwm_low_ticks(),
                 (unsigned int)laser_pwm_last_requested_power(), (unsigned int)laser_pwm_last_effective_power(),
                 motion_executor_max_sample_late_us(), motion_executor_late_sample_count(),
                 motion_executor_missed_sample_count(), motion_executor_motion_segment_count(),
                 motion_executor_short_segment_count());
        stream_send_str(src, buf);
    } else if (strcmp(line, "$H") == 0) {
        wait_motion_idle(src, MOTION_END_DRAIN_TIMEOUT_MS);
        gcode_processor_set_origin();
        motion_executor_set_origin();
        send_ok(src);
    } else if (strcmp(line, "$C") == 0) {
        stream_send_str(src, "[GC:G0 G54 G17 G21 G90 G94 M5]\r\nok\r\n");
    } else if (strcmp(line, "$X") == 0) {
        motion_executor_clear_abort();
        rx_mode_set(RX_MODE_UART_DIRECT);
        safety_force_laser_off();
        stream_send_str(src, "[MSG:Alarm lock cleared]\r\nok\r\n");
    } else if (strcmp(line, "$") == 0) {
        stream_send_str(src, "$G - View gcode parser state\r\n");
        stream_send_str(src, "$I - View build info\r\n");
        stream_send_str(src, "$D - View motion debug state\r\n");
        stream_send_str(src, "$X - Kill alarm lock\r\n");
        stream_send_str(src, "$H - Set origin\r\n");
        send_ok(src);
    } else {
        send_ok(src);
    }

    return true;
}

static void wait_motion_idle(rx_stream_src_t src, uint32_t timeout_ms)
{
    unsigned long start = (unsigned long)uapi_systick_get_ms();
    unsigned long last_status_ms = 0;

    while (motion_executor_is_busy()) {
        if (((unsigned long)uapi_systick_get_ms() - start) >= timeout_ms) {
            break;
        }
        send_wait_status(src, &last_status_ms);
        osal_msleep(1);
    }
}

static void handle_emergency_stop(rx_stream_src_t src)
{
    unused(src);
    safety_abort_all();
}

static bool handle_realtime_char(rx_stream_src_t src, uint8_t ch)
{
    switch (ch) {
        case '?':
            send_status_report(src);
            return true;
        case '!':
            handle_emergency_stop(src);
            stream_send_str(src, "[MSG:Emergency stop]\r\nok\r\n");
            return true;
        case '~':
            motion_executor_clear_abort();
            rx_mode_set(RX_MODE_UART_DIRECT);
            safety_force_laser_off();
            stream_send_str(src, "[MSG:Resume]\r\nok\r\n");
            return true;
        case GRBL_RESET_CHAR:
            g_rx_pos = 0;
            handle_emergency_stop(src);
            wait_motion_idle(src, 100);
            gcode_processor_init();
            motion_executor_set_origin();
            motion_executor_clear_abort();
            rx_mode_set(RX_MODE_UART_DIRECT);
            safety_force_laser_off();
            send_grbl_startup(src, "soft-reset");
            return true;
        default:
            return false;
    }
}

static void wait_motion_queue_watermark(rx_stream_src_t src)
{
    while (motion_executor_queue_depth() >= MOTION_QUEUE_OK_WATERMARK) {
        send_periodic_status(src);
        osal_msleep(1);
    }
}

static void execute_gcode_line(rx_stream_src_t src, const char *line, int len)
{
    motion_cmd_t cmds[4];
    int cmd_count = 0;
    bool drain_before_ok = line_contains_mcode(line, 5);

    if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        for (int i = 0; i < cmd_count; i++) {
            if (!enqueue_motion_cmd(src, &cmds[i])) {
                return;
            }
        }
        if (cmd_count > 0) {
            wait_motion_queue_watermark(src);
        }
        if (drain_before_ok) {
            wait_motion_idle(src, MOTION_END_DRAIN_TIMEOUT_MS);
            safety_force_laser_off();
        }
    }

    send_ok(src);
}

static void process_line(rx_stream_src_t src, const char *line, int len)
{
    if (len == 0) {
        return;
    }

    if (strcmp(line, "?") == 0) {
        send_status_report(src);
        return;
    }

    if (strcmp(line, "!") == 0 || strcmp(line, "$STOP") == 0 || strcmp(line, "M112") == 0) {
        handle_emergency_stop(src);
        send_ok(src);
        return;
    }

    if (handle_dollar_command(src, line)) {
        return;
    }

    if (len >= RX_LINE_MAX - 1) {
        send_error(src, 1);
        return;
    }

    execute_gcode_line(src, line, len);
}

void rx_stream_init(void)
{
    g_rx_pos = 0;
#if RX_STREAM_STATUS_PERIODIC
    g_last_status_ms = 0;
#endif
    osal_printk("[RX_STREAM] init ok\r\n");
}

void rx_stream_on_ready(rx_stream_src_t src)
{
    send_grbl_startup(src, "boot");
}

void rx_stream_on_poll(rx_stream_src_t src)
{
    send_periodic_status(src);
}

void rx_stream_on_byte(rx_stream_src_t src, uint8_t byte)
{
    if (handle_realtime_char(src, byte)) {
        return;
    }

    if (byte == '\n' || byte == '\r') {
        if (g_rx_pos > 0) {
            g_rx_line[g_rx_pos] = '\0';
            process_line(src, g_rx_line, g_rx_pos);
            g_rx_pos = 0;
        }
    } else if (g_rx_pos < RX_LINE_MAX - 1) {
        g_rx_line[g_rx_pos++] = (char)byte;
    }
}
