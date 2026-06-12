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
#include "crc16.h"
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
#include <stdlib.h>
#include <string.h>

/* G-code processing - same as ws63_laser_single */
#define RX_LINE_MAX 128
#define RX_STREAM_BUF_SIZE 8192
#define GRBL_RESET_CHAR 0x18

/* Forward declarations */
static void process_line(const char *line, int len);
static void send_ok(void);
static void send_error(int code);
static void send_response(const char *str);
static void wait_motion_idle(uint32_t timeout_ms);

static char g_rx_line[RX_LINE_MAX];
static int g_rx_pos = 0;
static uint8_t g_stream_buf[RX_STREAM_BUF_SIZE];
static uint16_t g_stream_head = 0;
static uint16_t g_stream_tail = 0;
static volatile bool g_stream_overflow = false;
static bool g_stream_ready = false;
static osal_mutex g_stream_mutex;
static osal_semaphore g_stream_sem;
static volatile uint16_t g_last_accepted_seq = 0;
#if LASER_SINGLE_STATUS_PERIODIC
static unsigned long g_last_status_ms = 0;
#endif

/* Send response via both UART (for debug) and SLE (to transmitter) */
static void send_response(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    if (len == 0) return;

    /* Send via SLE back to transmitter first; this is the GRBL control path. */
    if (sle_receiver_is_connected()) {
        sle_receiver_send_response(str, (uint16_t)len);
    }

#if SLE_LASER_LOCAL_UART_ECHO
    uapi_uart_write(LASER_UART_BUS, (const uint8_t *)str, len, 0);
#endif
}

static uint8_t motion_queue_free_count(void)
{
    uint16_t depth = motion_executor_queue_depth();
    uint16_t usable = (MOTION_QUEUE_SIZE > 0U) ? (uint16_t)(MOTION_QUEUE_SIZE - 1U) : 0U;
    if (depth >= usable) {
        return 0;
    }

    uint16_t free_count = (uint16_t)(usable - depth);
    return (free_count > 255U) ? 255U : (uint8_t)free_count;
}

static uint8_t runtime_status(void)
{
    return motion_executor_is_busy() ? STATUS_RUNNING : STATUS_IDLE;
}

static void send_status_packet(uint8_t status, uint8_t error_code, uint16_t ack_seq)
{
    status_full_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.base.status = status;
    pkt.base.error_code = error_code;
    pkt.base.ack_seq = ack_seq;
    pkt.base.queue_free = motion_queue_free_count();
    pkt.cur_x = (float)motion_executor_get_x();
    pkt.cur_y = (float)motion_executor_get_y();
    status_pkt_set_crc(&pkt.base);

    if (sle_receiver_is_connected()) {
        (void)sle_receiver_send_response(&pkt, (uint16_t)sizeof(pkt));
    }
}

static bool validate_motion_packet(const motion_cmd_t *cmd, uint8_t *error_code)
{
    if (error_code != NULL) {
        *error_code = STATUS_ERR_NONE;
    }

    if (cmd == NULL) {
        if (error_code != NULL) {
            *error_code = STATUS_ERR_INVALID_CMD;
        }
        return false;
    }

    switch (cmd->cmd) {
        case CMD_G0_MOVE:
        case CMD_G1_MOVE:
        case CMD_LASER_ON:
        case CMD_LASER_OFF:
        case CMD_SET_ORIGIN:
        case CMD_EMERGENCY_STOP:
        case CMD_HEARTBEAT:
            return true;
        default:
            if (error_code != NULL) {
                *error_code = STATUS_ERR_INVALID_CMD;
            }
            return false;
    }
}

void sle_motion_cmd_received(const uint8_t *data, uint16_t len)
{
    motion_cmd_t cmd;
    uint8_t validate_err = STATUS_ERR_NONE;

    if (data == NULL || len != sizeof(motion_cmd_t)) {
        send_status_packet(STATUS_ERROR, STATUS_ERR_INVALID_PARAM, g_last_accepted_seq);
        return;
    }

    memcpy(&cmd, data, sizeof(cmd));
    if (!motion_cmd_check_crc(&cmd)) {
        osal_printk("[rx] motion crc error seq=%u\r\n", cmd.seq);
        send_status_packet(STATUS_ERROR, STATUS_ERR_CRC, g_last_accepted_seq);
        return;
    }

    if (!validate_motion_packet(&cmd, &validate_err)) {
        osal_printk("[rx] invalid motion cmd=0x%x seq=%u err=%u\r\n", cmd.cmd, cmd.seq, validate_err);
        send_status_packet(STATUS_ERROR, validate_err, g_last_accepted_seq);
        return;
    }

    if (cmd.cmd == CMD_HEARTBEAT) {
        send_status_packet(runtime_status(), STATUS_ERR_NONE, g_last_accepted_seq);
        return;
    }

    if (cmd.seq == g_last_accepted_seq) {
        send_status_packet(runtime_status(), STATUS_ERR_NONE, g_last_accepted_seq);
        return;
    }

    if (cmd.cmd == CMD_EMERGENCY_STOP) {
        motion_executor_execute(&cmd);
        g_last_accepted_seq = cmd.seq;
        send_status_packet(STATUS_ERROR, STATUS_ERR_ESTOP, g_last_accepted_seq);
        return;
    }

    if (!motion_executor_enqueue(&cmd)) {
        send_status_packet(STATUS_ERROR, STATUS_ERR_QUEUE_FULL, g_last_accepted_seq);
        return;
    }

    g_last_accepted_seq = cmd.seq;
    send_status_packet(STATUS_RUNNING, STATUS_ERR_NONE, g_last_accepted_seq);
}

void sle_motion_link_reset(void)
{
    g_last_accepted_seq = 0;
}

static uint16_t stream_next(uint16_t value)
{
    return (uint16_t)((value + 1U) % RX_STREAM_BUF_SIZE);
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

/* SLE G-code callback - called from SLE stack, keep it short. */
void sle_gcode_stream_received(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || !g_stream_ready) {
        return;
    }

    bool pushed = false;
    osal_mutex_lock(&g_stream_mutex);
    for (uint16_t i = 0; i < len; i++) {
        uint16_t next = stream_next(g_stream_head);
        if (next == g_stream_tail) {
            g_stream_overflow = true;
            break;
        }
        g_stream_buf[g_stream_head] = data[i];
        g_stream_head = next;
        pushed = true;
    }
    osal_mutex_unlock(&g_stream_mutex);

    if (pushed) {
        osal_sem_up(&g_stream_sem);
    }
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
    send_response("\r\nGrbl 1.1f ['$' for help]\r\n");
    snprintf(buf, sizeof(buf), "[MSG:startup source=%s uptime=%lums]\r\n",
             source, (unsigned long)uapi_systick_get_ms());
    send_response(buf);
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
    if (motion_executor_is_busy()) return false;
    unsigned long last = motion_executor_last_activity_ms();
    if (last == 0) return true;
    return ((unsigned long)uapi_systick_get_ms() - last) > ACTIVITY_TIMEOUT_MS;
}

static void send_status_report(void)
{
    char buf[128];
    const char *state = machine_is_idle() ? "Idle" : "Run";
    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d|Ln:%lu>\r\n", state,
             motion_executor_get_x(), motion_executor_get_y(),
             (int)gcode_processor_get_feed_rate(),
             (int)gcode_processor_get_laser_power(), gcode_processor_get_line_count());
    send_response(buf);
}

static void send_periodic_status(void)
{
#if LASER_SINGLE_STATUS_PERIODIC
    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - g_last_status_ms) >= STATUS_INTERVAL_MS) {
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
    if ((now - *last_status_ms) >= STATUS_INTERVAL_MS) {
        send_status_report();
        *last_status_ms = now;
    }
}

static bool handle_dollar_command(const char *line)
{
    if (line[0] != '$') return false;

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
        if (!enqueue_motion_cmd(&cmd)) return true;

        cmd.cmd = CMD_G1_MOVE;
        cmd.feed_rate = (float)FRAME_FEED_RATE;
        cmd.flags = FLAG_LASER_ON;
        cmd.laser_pwr = FRAME_LASER_POWER;
        cmd.target_x = (float)GALVO_X_MAX_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) return true;
        cmd.target_x = (float)GALVO_X_MAX_MM;
        cmd.target_y = (float)GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) return true;
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) return true;
        cmd.target_x = (float)GALVO_X_MIN_MM;
        cmd.target_y = (float)GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) return true;

        cmd.cmd = CMD_LASER_OFF;
        cmd.flags = 0;
        cmd.laser_pwr = 0;
        if (!enqueue_motion_cmd(&cmd)) return true;
        send_ok();
    } else if (strcmp(line, "$I") == 0) {
        send_response("[VER:1.1f.WS63_SLE:]\r\n[OPT:V,15,128]\r\nok\r\n");
    } else if (strcmp(line, "$G") == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n",
                 gcode_processor_is_absolute_mode() ? 90 : 91,
                 gcode_processor_laser_is_enabled() ? 3 : 5,
                 (int)gcode_processor_get_feed_rate(),
                 (int)gcode_processor_get_laser_power());
        send_response(buf);
    } else if (strcmp(line, "$D") == 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "[MSG:motion busy=%d queue=%u abort=%d worker=%d enq=%lu exe=%lu x=%.3f y=%.3f laser=%d power=%u late_max=%lu late_cnt=%lu slip=%lu seg=%lu short=%lu]\r\nok\r\n",
                 motion_executor_is_busy() ? 1 : 0, (unsigned int)motion_executor_queue_depth(),
                 motion_executor_abort_requested() ? 1 : 0, motion_executor_worker_started() ? 1 : 0,
                 motion_executor_enqueued_count(), motion_executor_executed_count(),
                 motion_executor_get_x(), motion_executor_get_y(), laser_is_enabled() ? 1 : 0,
                 (unsigned int)laser_get_power(), motion_executor_max_sample_late_us(),
                 motion_executor_late_sample_count(), motion_executor_missed_sample_count(),
                 motion_executor_motion_segment_count(), motion_executor_short_segment_count());
        send_response(buf);
    } else if (strcmp(line, "$H") == 0) {
        wait_motion_idle(MOTION_END_DRAIN_TIMEOUT_MS);
        gcode_processor_set_origin();
        motion_executor_set_origin();
        send_ok();
    } else if (strcmp(line, "$C") == 0) {
        send_response("[GC:G0 G54 G17 G21 G90 G94 M5]\r\nok\r\n");
    } else if (strcmp(line, "$$") == 0) {
        send_response("$0=10\r\n$1=25\r\n$2=0\r\n$3=0\r\n$4=0\r\n$5=0\r\n$6=0\r\n");
        send_response("$10=1\r\n$11=0.010\r\n$12=0.002\r\n$13=0\r\n$20=0\r\n$21=0\r\n$22=0\r\n$23=0\r\n");
        send_response("$24=25.000\r\n$25=500.000\r\n$26=250\r\n$27=1.000\r\n$30=1000\r\n$31=0\r\n$32=0\r\nok\r\n");
    } else if (strcmp(line, "$") == 0) {
        send_response("$G - View gcode parser state\r\n");
        send_response("$I - View build info\r\n");
        send_response("$D - View motion debug state\r\n");
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
            return true;
        case '~':
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

static void wait_motion_queue_watermark(void)
{
    while (motion_executor_queue_depth() >= MOTION_QUEUE_OK_WATERMARK) {
        send_periodic_status();
        osal_msleep(1);
    }
}

static void execute_gcode_line(const char *line, int len)
{
    motion_cmd_t cmds[4];
    int cmd_count = 0;
    bool drain_before_ok = line_contains_mcode(line, 5);

    if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        for (int i = 0; i < cmd_count; i++) {
            if (!enqueue_motion_cmd(&cmds[i])) return;
        }
        if (cmd_count > 0) {
            wait_motion_queue_watermark();
        }
        if (drain_before_ok) {
            wait_motion_idle(MOTION_END_DRAIN_TIMEOUT_MS);
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

static void process_stream_char(uint8_t ch)
{
    if (handle_realtime_char(ch)) {
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

static int gcode_stream_task(void *arg)
{
    unused(arg);
    osal_printk("[rx] gcode stream task started\r\n");

    while (1) {
        if (osal_sem_down(&g_stream_sem) != OSAL_SUCCESS) {
            osal_msleep(1);
            continue;
        }

        if (g_stream_overflow) {
            osal_mutex_lock(&g_stream_mutex);
            g_stream_head = 0;
            g_stream_tail = 0;
            g_stream_overflow = false;
            osal_mutex_unlock(&g_stream_mutex);
            g_rx_pos = 0;
            send_error(9);
        }

        uint8_t ch = 0;
        while (stream_pop_byte(&ch)) {
            process_stream_char(ch);
        }
    }

    return 0;
}

static errcode_t gcode_stream_init(void)
{
    g_stream_head = 0;
    g_stream_tail = 0;
    g_stream_overflow = false;
    g_rx_pos = 0;

    if (osal_mutex_init(&g_stream_mutex) != OSAL_SUCCESS ||
        osal_sem_init(&g_stream_sem, 0) != OSAL_SUCCESS) {
        osal_printk("[rx] stream sync init failed\r\n");
        return ERRCODE_FAIL;
    }

    g_stream_ready = true;
    return ERRCODE_SUCC;
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

    osal_printk("[rx] starting dac8562_init...\r\n");
    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] dac init failed: 0x%x\r\n", ret);
        return;
    }
    osal_printk("[rx] dac8562_init OK\r\n");

    osal_printk("[rx] starting laser_ctrl_init...\r\n");
    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] laser init failed: 0x%x\r\n", ret);
        return;
    }
    osal_printk("[rx] laser_ctrl_init OK\r\n");

    osal_printk("[rx] starting gcode_processor_init...\r\n");
    gcode_processor_init();
    osal_printk("[rx] gcode_processor_init OK\r\n");

    osal_printk("[rx] starting motion_executor_init...\r\n");
    motion_executor_init();
    osal_printk("[rx] motion_executor_init OK\r\n");

    osal_printk("[rx] starting stream buffer init...\r\n");
    ret = gcode_stream_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] stream init failed: 0x%x\r\n", ret);
        return;
    }
    osal_printk("[rx] stream buffer init OK\r\n");

    osal_kthread_lock();

    osal_printk("[rx] creating G-code stream task...\r\n");
    osal_task *task = osal_kthread_create(gcode_stream_task, NULL, "gcode_stream", TASK_STACK_SIZE_DEFAULT);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_UART);
        osal_kfree(task);
        osal_printk("[rx] G-code stream task created\r\n");
    } else {
        osal_printk("[rx] G-code stream task creation failed!\r\n");
    }

    /* SLE task */
    osal_printk("[rx] creating SLE task...\r\n");
    task = osal_kthread_create(sle_init_task, NULL, "sle_init", 0x2000);
    if (task != NULL) {
        osal_kthread_set_priority(task, 26);
        osal_kfree(task);
        osal_printk("[rx] SLE task created\r\n");
    } else {
        osal_printk("[rx] SLE task creation failed!\r\n");
    }

    osal_kthread_unlock();

    /* Motion executor task */
    osal_printk("[rx] starting motion executor task...\r\n");
    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[rx] motion task failed: 0x%x\r\n", ret);
    } else {
        osal_printk("[rx] motion task started\r\n");
    }

    send_grbl_startup("boot");
    osal_printk("[rx] ready, waiting for SLE connection...\r\n");
}

app_run(sle_laser_receiver_entry);
