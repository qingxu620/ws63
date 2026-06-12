/**
 * @file stream_io.c
 * @brief Grbl-compatible byte stream processor for the SLE bridge receiver.
 */
#include "stream_io.h"
#include "bridge_rx_stats.h"
#include "common_def.h"
#include "config.h"
#include "gcode_parser.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "preserve.h"
#include "soc_osal.h"
#include "systick.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_LINE_MAX 128
#define GRBL_RESET_CHAR 0x18

static stream_io_write_cb_t g_write_cb = NULL;
static char g_rx_line[RX_LINE_MAX];
static int g_rx_pos = 0;

static uint8_t g_stream_buf[SLE_BRIDGE_STREAM_BUF_SIZE];
static uint16_t g_stream_head = 0;
static uint16_t g_stream_tail = 0;
static volatile bool g_stream_overflow = false;
static volatile bool g_startup_pending = false;
static volatile bool g_host_startup_pending = false;
static bool g_stream_ready = false;
static osal_mutex g_stream_mutex;
static osal_semaphore g_stream_sem;

static void wait_motion_idle(uint32_t timeout_ms);

static const char *motion_cmd_name(uint8_t cmd)
{
    switch (cmd) {
        case CMD_G0_MOVE:
            return "G0";
        case CMD_G1_MOVE:
            return "G1";
        case CMD_LASER_ON:
            return "M3";
        case CMD_LASER_OFF:
            return "M5";
        case CMD_SET_ORIGIN:
            return "ORIGIN";
        case CMD_EMERGENCY_STOP:
            return "ESTOP";
        default:
            return "OTHER";
    }
}

static void stream_write(const void *data, uint16_t len)
{
    if (data == NULL || len == 0 || g_write_cb == NULL) {
        return;
    }
    unsigned long start_ms = (unsigned long)uapi_systick_get_ms();
    errcode_t ret = g_write_cb(data, len);
#if SLE_BRIDGE_TIMING_VERBOSE
    osal_printk("[BRIDGE_TIMING_RX_RESP] resp_len=%u write_ms=%lu ret=0x%x\r\n",
                (unsigned int)len, (unsigned long)uapi_systick_get_ms() - start_ms,
                (unsigned int)ret);
#else
    unused(start_ms);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[bridge rx] response send failed ret=0x%x len=%u\r\n",
                    (unsigned int)ret, (unsigned int)len);
    }
#endif
}

static void stream_send_str(const char *str)
{
    if (str == NULL) {
        return;
    }
    stream_write(str, (uint16_t)strlen(str));
}

static void send_ok(void)
{
    stream_send_str("ok\r\n");
}

static void send_error(int code)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "error:%d\r\n", code);
    stream_send_str(buf);
}

static void send_grbl_startup(const char *source)
{
    char buf[128];
    stream_send_str("\r\nGrbl 1.1f ['$' for help]\r\n");
    snprintf(buf, sizeof(buf), "[MSG:WS63 SLE Bridge RX ready source=%s uptime=%lums reset=0x%04x count=%u]\r\n",
             source, (unsigned long)uapi_systick_get_ms(), (unsigned int)get_cpu_utils_reset_cause(),
             get_cpu_utils_reset_count());
    stream_send_str(buf);
}

static bool enqueue_motion_cmd(const motion_cmd_t *cmd)
{
    if (!motion_executor_enqueue(cmd)) {
        send_error(9);
        return false;
    }
    return true;
}

static bool parsed_line_contains_gcode(const gcode_line_t *gc, int expected_code)
{
    const char *p = gc->line;
    while ((p = strchr(p, 'G')) != NULL) {
        if ((p == gc->line || !isalpha((unsigned char)*(p - 1))) && atoi(p + 1) == expected_code) {
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

static void send_settings_report(void)
{
    char buf[64];

    stream_send_str("$0=10\r\n");
    stream_send_str("$1=25\r\n");
    stream_send_str("$2=0\r\n");
    stream_send_str("$3=0\r\n");
    stream_send_str("$4=0\r\n");
    stream_send_str("$5=0\r\n");
    stream_send_str("$6=0\r\n");
    stream_send_str("$10=1\r\n");
    stream_send_str("$11=0.010\r\n");
    stream_send_str("$12=0.002\r\n");
    stream_send_str("$13=0\r\n");
    stream_send_str("$20=0\r\n");
    stream_send_str("$21=0\r\n");
    stream_send_str("$22=0\r\n");
    stream_send_str("$23=0\r\n");
    stream_send_str("$24=25.000\r\n");
    stream_send_str("$25=500.000\r\n");
    stream_send_str("$26=250\r\n");
    stream_send_str("$27=1.000\r\n");
    stream_send_str("$30=1000\r\n");
    stream_send_str("$31=0\r\n");
    stream_send_str("$32=1\r\n");
    stream_send_str("$100=80.000\r\n");
    stream_send_str("$101=80.000\r\n");
    stream_send_str("$102=250.000\r\n");
    stream_send_str("$110=8000.000\r\n");
    stream_send_str("$111=8000.000\r\n");
    stream_send_str("$112=500.000\r\n");
    stream_send_str("$120=1000.000\r\n");
    stream_send_str("$121=1000.000\r\n");
    stream_send_str("$122=10.000\r\n");
    snprintf(buf, sizeof(buf), "$130=%.3f\r\n", GALVO_WORK_AREA_X_MM);
    stream_send_str(buf);
    snprintf(buf, sizeof(buf), "$131=%.3f\r\n", GALVO_WORK_AREA_Y_MM);
    stream_send_str(buf);
    stream_send_str("$132=0.000\r\n");
    send_ok();
}

static void send_coordinate_report(void)
{
    stream_send_str("[G54:0.000,0.000,0.000]\r\n");
    stream_send_str("[G55:0.000,0.000,0.000]\r\n");
    stream_send_str("[G56:0.000,0.000,0.000]\r\n");
    stream_send_str("[G57:0.000,0.000,0.000]\r\n");
    stream_send_str("[G58:0.000,0.000,0.000]\r\n");
    stream_send_str("[G59:0.000,0.000,0.000]\r\n");
    stream_send_str("[G28:0.000,0.000,0.000]\r\n");
    stream_send_str("[G30:0.000,0.000,0.000]\r\n");
    stream_send_str("[G92:0.000,0.000,0.000]\r\n");
    stream_send_str("[TLO:0.000]\r\n");
    stream_send_str("[PRB:0.000,0.000,0.000:0]\r\n");
    send_ok();
}

static void send_startup_blocks(void)
{
    stream_send_str("$N0=\r\n");
    stream_send_str("$N1=\r\n");
    send_ok();
}

static void send_status_report(void)
{
    char buf[128];
    const char *state = machine_is_idle() ? "Idle" : "Run";
    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d|Ln:%lu>\r\n", state,
             motion_executor_get_x(), motion_executor_get_y(), (int)gcode_processor_get_feed_rate(),
             (int)gcode_processor_get_laser_power(), gcode_processor_get_line_count());
    stream_send_str(buf);
}

static void send_wait_status(unsigned long *last_status_ms)
{
    if (last_status_ms == NULL) {
        return;
    }

    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - *last_status_ms) >= STATUS_INTERVAL_MS) {
        send_status_report();
        *last_status_ms = now;
    }
}

static bool handle_dollar_command(const char *line)
{
    char buf[768];

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
            send_error(2);
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
            send_error(2);
            return true;
        }

        motion_cmd_t cmd = {0};
        cmd.cmd = CMD_G1_MOVE;
        cmd.target_x = (float)target_x;
        cmd.target_y = (float)target_y;
        cmd.feed_rate = (float)feed_rate;
        if (enqueue_motion_cmd(&cmd)) {
            send_ok();
        }
    } else if (strcmp(line, "$FRAME") == 0) {
        motion_cmd_t cmd = {0};

        cmd.cmd = CMD_G0_MOVE;
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }

        cmd.cmd = CMD_G1_MOVE;
        cmd.feed_rate = (float)FRAME_FEED_RATE;
        cmd.flags = FLAG_LASER_ON;
        cmd.laser_pwr = FRAME_LASER_POWER;
        cmd.target_x = (float)GALVO_X_MAX_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)GALVO_X_MAX_MM;
        cmd.target_y = (float)GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }

        cmd.cmd = CMD_LASER_OFF;
        cmd.flags = 0;
        cmd.laser_pwr = 0;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        send_ok();
    } else if (strcmp(line, "$$") == 0) {
        send_settings_report();
    } else if (strcmp(line, "$#") == 0) {
        send_coordinate_report();
    } else if (strcmp(line, "$N") == 0) {
        send_startup_blocks();
    } else if (strcmp(line, "$I") == 0) {
        stream_send_str("[VER:1.1f.20260612:WS63_SLE_BRIDGE]\r\n");
        stream_send_str("[OPT:V,15,128]\r\n");
        send_ok();
    } else if (strcmp(line, "$G") == 0) {
        snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d M9 T0 F%d S%d]\r\nok\r\n",
                 gcode_processor_is_absolute_mode() ? 90 : 91, gcode_processor_laser_is_enabled() ? 3 : 5,
                 (int)gcode_processor_get_feed_rate(), (int)gcode_processor_get_laser_power());
        stream_send_str(buf);
    } else if (strcmp(line, "$D") == 0) {
        bridge_rx_stats_t rx_stats = {0};
        bridge_rx_stats_get(&rx_stats);
        snprintf(buf, sizeof(buf),
                 "[MSG:bridge_rx motion busy=%d queue=%u qmax=%u qwait=%lu qtimeout=%lu abort=%d worker=%d enq=%lu exe=%lu x=%.3f y=%.3f laser=%d power=%u late_max=%lu late_cnt=%lu slip=%lu seg=%lu short=%lu resp=%lu notify_retry=%lu notify_fail=%lu max_resp_ms=%lu]\r\nok\r\n",
                 motion_executor_is_busy() ? 1 : 0, (unsigned int)motion_executor_queue_depth(),
                 (unsigned int)motion_executor_max_queue_depth(), motion_executor_queue_wait_count(),
                 motion_executor_enqueue_timeout_count(), motion_executor_abort_requested() ? 1 : 0,
                 motion_executor_worker_started() ? 1 : 0,
                 motion_executor_enqueued_count(), motion_executor_executed_count(),
                 motion_executor_get_x(), motion_executor_get_y(), laser_is_enabled() ? 1 : 0,
                 (unsigned int)laser_get_power(), motion_executor_max_sample_late_us(),
                 motion_executor_late_sample_count(), motion_executor_missed_sample_count(),
                 motion_executor_motion_segment_count(), motion_executor_short_segment_count(),
                 rx_stats.resp_generated, rx_stats.notify_retry, rx_stats.notify_fail,
                 rx_stats.max_resp_delay_ms);
        stream_send_str(buf);
    } else if (strcmp(line, "$H") == 0) {
        wait_motion_idle(MOTION_END_DRAIN_TIMEOUT_MS);
        gcode_processor_set_origin();
        motion_executor_set_origin();
        send_ok();
    } else if (strcmp(line, "$X") == 0) {
        motion_executor_clear_abort();
        stream_send_str("[MSG:Caution: Unlocked]\r\n");
        send_ok();
    } else if (strcmp(line, "$C") == 0) {
        stream_send_str("[GC:G0 G54 G17 G21 G90 G94 M5]\r\nok\r\n");
    } else if (strcmp(line, "$RST=$") == 0 || strcmp(line, "$RST#") == 0 || strcmp(line, "$RST*") == 0) {
        send_ok();
    } else if (strcmp(line, "$") == 0) {
        stream_send_str("$G - View gcode parser state\r\n");
        stream_send_str("$I - View build info\r\n");
        stream_send_str("$$ - View Grbl settings\r\n");
        stream_send_str("$# - View coordinate parameters\r\n");
        stream_send_str("$N - View startup blocks\r\n");
        stream_send_str("$D - View bridge receiver debug state\r\n");
        stream_send_str("$X - Kill alarm lock\r\n");
        stream_send_str("$H - Set origin\r\n");
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

static void wait_motion_idle(uint32_t timeout_ms)
{
    unsigned long start = (unsigned long)uapi_systick_get_ms();
    unsigned long last_status_ms = 0;

    while (motion_executor_is_busy()) {
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
            motion_executor_request_abort();
            motion_executor_flush();
            laser_force_off();
            send_status_report();
            return true;
        case '~':
            motion_executor_clear_abort();
            send_status_report();
            return true;
        case GRBL_RESET_CHAR:
            g_rx_pos = 0;
            handle_emergency_stop();
            wait_motion_idle(100);
            gcode_processor_init();
            motion_executor_set_origin();
            send_grbl_startup("soft-reset");
            return true;
        default:
            return false;
    }
}

static const char *execute_gcode_line(const char *line, int len)
{
    motion_cmd_t cmds[4];
    int cmd_count = 0;
    const char *first_cmd = "NONE";

    if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        if (cmd_count > 0) {
            first_cmd = motion_cmd_name(cmds[0].cmd);
        }
        for (int i = 0; i < cmd_count; i++) {
            if (cmds[i].cmd == CMD_LASER_OFF) {
                laser_force_off();
            }
            if (!enqueue_motion_cmd(&cmds[i])) {
                return "ENQ_FAIL";
            }
        }
    }

    send_ok();
    return first_cmd;
}

static void process_line(const char *line, int len)
{
    unsigned long start_ms = (unsigned long)uapi_systick_get_ms();

    if (len == 0) {
        return;
    }

    if (strcmp(line, "?") == 0) {
        send_status_report();
#if SLE_BRIDGE_TIMING_VERBOSE
        osal_printk("[BRIDGE_TIMING_RX] line=\"?\" rx_process_ms=%lu queue=%u abort=%d\r\n",
                    (unsigned long)uapi_systick_get_ms() - start_ms,
                    (unsigned int)motion_executor_queue_depth(),
                    motion_executor_abort_requested() ? 1 : 0);
#endif
        return;
    }

    if (strcmp(line, "!") == 0 || strcmp(line, "$STOP") == 0 || strcmp(line, "M112") == 0) {
        handle_emergency_stop();
        send_ok();
#if SLE_BRIDGE_TIMING_VERBOSE
        osal_printk("[BRIDGE_TIMING_RX] line=\"%s\" rx_process_ms=%lu queue=%u abort=%d\r\n",
                    line, (unsigned long)uapi_systick_get_ms() - start_ms,
                    (unsigned int)motion_executor_queue_depth(),
                    motion_executor_abort_requested() ? 1 : 0);
#endif
        return;
    }

    if (handle_dollar_command(line)) {
#if SLE_BRIDGE_TIMING_VERBOSE
        osal_printk("[BRIDGE_TIMING_RX] line=\"%s\" rx_process_ms=%lu queue=%u abort=%d\r\n",
                    line, (unsigned long)uapi_systick_get_ms() - start_ms,
                    (unsigned int)motion_executor_queue_depth(),
                    motion_executor_abort_requested() ? 1 : 0);
#endif
        return;
    }

    if (len >= RX_LINE_MAX - 1) {
        send_error(1);
#if SLE_BRIDGE_TIMING_VERBOSE
        osal_printk("[BRIDGE_TIMING_RX] line=\"%s\" rx_process_ms=%lu error=1\r\n",
                    line, (unsigned long)uapi_systick_get_ms() - start_ms);
#endif
        return;
    }

    const char *cmd_name = execute_gcode_line(line, len);
#if SLE_BRIDGE_TIMING_VERBOSE
    osal_printk("[BRIDGE_TIMING_RX] line=\"%s\" cmd=%s rx_process_ms=%lu queue=%u qwait=%lu qtimeout=%lu abort=%d\r\n",
                line, cmd_name, (unsigned long)uapi_systick_get_ms() - start_ms,
                (unsigned int)motion_executor_queue_depth(), motion_executor_queue_wait_count(),
                motion_executor_enqueue_timeout_count(), motion_executor_abort_requested() ? 1 : 0);
#else
    unused(start_ms);
    unused(cmd_name);
#endif
}

static uint16_t stream_next(uint16_t value)
{
    return (uint16_t)((value + 1U) % SLE_BRIDGE_STREAM_BUF_SIZE);
}

static bool stream_push_byte(uint8_t ch)
{
    if (!g_stream_ready) {
        return false;
    }

    bool pushed = false;
    osal_mutex_lock(&g_stream_mutex);
    uint16_t next = stream_next(g_stream_head);
    if (next == g_stream_tail) {
        g_stream_overflow = true;
    } else {
        g_stream_buf[g_stream_head] = ch;
        g_stream_head = next;
        pushed = true;
    }
    osal_mutex_unlock(&g_stream_mutex);
    return pushed;
}

static bool stream_pop_byte(uint8_t *ch)
{
    if (ch == NULL || !g_stream_ready) {
        return false;
    }

    bool ok = false;
    osal_mutex_lock(&g_stream_mutex);
    if (g_stream_tail != g_stream_head) {
        *ch = g_stream_buf[g_stream_tail];
        g_stream_tail = stream_next(g_stream_tail);
        ok = true;
    }
    osal_mutex_unlock(&g_stream_mutex);
    return ok;
}

void stream_io_receive(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || !g_stream_ready) {
        return;
    }

    bool pushed = false;
    for (uint16_t i = 0; i < len; i++) {
        pushed = stream_push_byte(data[i]) || pushed;
        if (g_stream_overflow) {
            break;
        }
    }

    if (pushed || g_stream_overflow) {
        osal_sem_up(&g_stream_sem);
    }
}

void stream_io_notify_connected(void)
{
    motion_executor_clear_abort();
    laser_force_off();
    g_startup_pending = false;
    g_host_startup_pending = true;
}

void stream_io_notify_disconnected(void)
{
    g_startup_pending = false;
    g_host_startup_pending = false;
    g_rx_pos = 0;
    motion_executor_request_abort();
    motion_executor_flush();
    laser_force_off();
}

static void process_stream_char(uint8_t ch)
{
    if (g_host_startup_pending && ch != '\r' && ch != '\n' && ch != GRBL_RESET_CHAR) {
        g_host_startup_pending = false;
        send_grbl_startup("host-sync");
    }

    if (handle_realtime_char(ch)) {
        if (ch == GRBL_RESET_CHAR) {
            g_host_startup_pending = false;
        }
        return;
    }

    if (ch == '\n' || ch == '\r') {
        if (g_rx_pos > 0) {
            g_rx_line[g_rx_pos] = '\0';
            process_line(g_rx_line, g_rx_pos);
            g_rx_pos = 0;
        }
        return;
    }

    if (g_rx_pos < RX_LINE_MAX - 1) {
        g_rx_line[g_rx_pos++] = (char)ch;
    } else {
        g_rx_pos = 0;
        send_error(1);
    }
}

int stream_io_task(void *arg)
{
    unused(arg);
    osal_printk("[bridge rx] stream task started\r\n");

    while (1) {
        if (osal_sem_down(&g_stream_sem) != OSAL_SUCCESS) {
            osal_msleep(1);
            continue;
        }

        if (g_startup_pending) {
            g_startup_pending = false;
            osal_msleep(250);
            send_grbl_startup("sle-connect");
        }

        if (g_stream_overflow) {
            osal_mutex_lock(&g_stream_mutex);
            g_stream_head = 0;
            g_stream_tail = 0;
            g_stream_overflow = false;
            osal_mutex_unlock(&g_stream_mutex);
            g_rx_pos = 0;
            stream_send_str("[MSG:bridge rx stream overflow]\r\n");
            send_error(9);
        }

        uint8_t ch = 0;
        while (stream_pop_byte(&ch)) {
            process_stream_char(ch);
        }
    }

    return 0;
}

errcode_t stream_io_init(stream_io_write_cb_t write_cb)
{
    g_write_cb = write_cb;
    g_stream_head = 0;
    g_stream_tail = 0;
    g_stream_overflow = false;
    g_startup_pending = false;
    g_host_startup_pending = false;
    g_rx_pos = 0;

    if (osal_mutex_init(&g_stream_mutex) != OSAL_SUCCESS ||
        osal_sem_init(&g_stream_sem, 0) != OSAL_SUCCESS) {
        osal_printk("[bridge rx] stream sync init failed\r\n");
        return ERRCODE_FAIL;
    }

    g_stream_ready = true;
    return ERRCODE_SUCC;
}
