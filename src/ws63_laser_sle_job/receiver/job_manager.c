/**
 * @file job_manager.c
 * @brief Structured job protocol state machine on the RX board.
 */
#include "job_manager.h"
#include "common_def.h"
#include "config.h"
#include "gcode_processor.h"
#include "job_cache.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "packet.h"
#include "protocol.h"
#include "sle_errcode.h"
#include "sle_job_server.h"
#include "soc_osal.h"
#include "systick.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOB_LINE_MAX 160
#define JOB_EXEC_TASK_STACK_SIZE 0x2000

static volatile sle_job_state_t g_state = JOB_STATE_IDLE;
static volatile bool g_abort_requested = false;
static uint16_t g_expected_seq = 1;
static uint16_t g_last_seq = 0;
static uint16_t g_resp_seq = 1;
static unsigned long g_executed_lines = 0;
static unsigned long g_packet_count = 0;
static unsigned long g_nack_count = 0;

static const char *state_name(sle_job_state_t state)
{
    switch (state) {
        case JOB_STATE_IDLE: return "IDLE";
        case JOB_STATE_RECEIVING_JOB: return "RECEIVING_JOB";
        case JOB_STATE_JOB_READY: return "JOB_READY";
        case JOB_STATE_EXECUTING: return "EXECUTING";
        case JOB_STATE_PAUSED: return "PAUSED";
        case JOB_STATE_ABORTED: return "ABORTED";
        case JOB_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static uint16_t next_resp_seq(void)
{
    uint16_t seq = g_resp_seq++;
    if (g_resp_seq == 0) {
        g_resp_seq = 1;
    }
    return seq;
}

static void send_packet(uint8_t type, const void *payload, uint16_t payload_len)
{
    uint8_t buf[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t out_len = 0;
    if (!sle_packet_encode(type, 0, next_resp_seq(), payload, payload_len,
                           buf, sizeof(buf), &out_len)) {
        osal_printk("[JOB_RX] encode response fail type=0x%02x\r\n", type);
        return;
    }
    errcode_t ret = sle_job_server_send_packet(buf, out_len);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[JOB_RX] send response fail type=0x%02x ret=0x%x\r\n", type, ret);
    }
}

static void send_ack(uint8_t ack_type, uint16_t ack_seq, sle_job_status_t status)
{
    ack_payload_t ack = {0};
    ack.ack_type = ack_type;
    ack.status = (uint8_t)status;
    ack.ack_seq = ack_seq;
    ack.job_id = job_cache_job_id();
    ack.offset = job_cache_received();
    ack.credit = job_cache_free();
    send_packet((status == JOB_STATUS_OK) ? PKT_ACK : PKT_NACK, &ack, sizeof(ack));
    if (status != JOB_STATUS_OK) {
        g_nack_count++;
    }
}

static void send_status(sle_job_status_t status)
{
    status_resp_payload_t resp = {0};
    resp.state = (uint8_t)g_state;
    resp.status = (uint8_t)status;
    resp.last_seq = g_last_seq;
    resp.job_id = job_cache_job_id();
    resp.received_size = job_cache_received();
    resp.total_size = job_cache_total_size();
    resp.cache_free = job_cache_free();
    resp.executed_lines = (uint32_t)g_executed_lines;
    send_packet(PKT_STATUS_RESP, &resp, sizeof(resp));
}

static bool seq_accepts(const sle_packet_view_t *pkt)
{
    if (pkt->type == PKT_EXEC_STOP || pkt->type == PKT_JOB_ABORT || pkt->type == PKT_STATUS_REQ) {
        return true;
    }
    if (pkt->type == PKT_JOB_BEGIN) {
        g_expected_seq = pkt->seq;
    }
    return pkt->seq == g_expected_seq;
}

static void seq_commit(uint16_t seq)
{
    g_last_seq = seq;
    g_expected_seq = (uint16_t)(seq + 1U);
    if (g_expected_seq == 0) {
        g_expected_seq = 1;
    }
}

static void wait_motion_idle(uint32_t timeout_ms)
{
    unsigned long start = (unsigned long)uapi_systick_get_ms();
    while (motion_executor_is_busy()) {
        if (((unsigned long)uapi_systick_get_ms() - start) >= timeout_ms) {
            break;
        }
        osal_msleep(1);
    }
}

void job_manager_safe_stop(const char *reason)
{
    osal_printk("[JOB_SAFE_STOP] reason=%s state=%s\r\n",
                (reason != NULL) ? reason : "unknown", state_name(g_state));
    g_abort_requested = true;
    motion_executor_request_abort();
    motion_executor_flush();
    motion_cmd_t cmd;
    gcode_processor_build_emergency_stop(&cmd);
    motion_executor_execute(&cmd);
    laser_force_off();
    g_state = JOB_STATE_ABORTED;
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

static void strip_line(char *line)
{
    char *semi = strchr(line, ';');
    if (semi != NULL) {
        *semi = '\0';
    }
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    char *start = line;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != line) {
        memmove(line, start, strlen(start) + 1);
    }
}

static bool execute_line(const char *line)
{
    motion_cmd_t cmds[4];
    int cmd_count = 0;
    bool drain_and_off = line_contains_mcode(line, 5);

    if (!gcode_process_line(line, (int)strlen(line), cmds, 4, &cmd_count)) {
        osal_printk("[JOB_EXEC] parse fail line=%lu text=\"%s\"\r\n", g_executed_lines + 1UL, line);
        return false;
    }
    for (int i = 0; i < cmd_count; i++) {
        while (motion_executor_queue_depth() >= MOTION_QUEUE_OK_WATERMARK && !g_abort_requested) {
            osal_msleep(1);
        }
        if (g_abort_requested) {
            return false;
        }
        if (!motion_executor_enqueue(&cmds[i])) {
            osal_printk("[JOB_EXEC] enqueue fail line=%lu cmd=%d\r\n", g_executed_lines + 1UL, cmds[i].cmd);
            return false;
        }
    }
    if (drain_and_off) {
        wait_motion_idle(MOTION_END_DRAIN_TIMEOUT_MS);
        laser_force_off();
    }
    return true;
}

static int job_exec_task(void *arg)
{
    unused(arg);
    const uint8_t *data = job_cache_data();
    uint32_t size = job_cache_received();
    uint32_t pos = 0;
    char line[JOB_LINE_MAX];
    uint16_t line_pos = 0;

    osal_printk("[JOB_EXEC] start job=%u size=%u\r\n",
                (unsigned int)job_cache_job_id(), (unsigned int)size);
    g_state = JOB_STATE_EXECUTING;
    g_abort_requested = false;
    g_executed_lines = 0;

    while (pos < size && !g_abort_requested) {
        char ch = (char)data[pos++];
        if (ch == '\r' || ch == '\n') {
            if (line_pos == 0) {
                continue;
            }
            line[line_pos] = '\0';
            strip_line(line);
            if (line[0] != '\0') {
                if (!execute_line(line)) {
                    job_manager_safe_stop("execute-line-fail");
                    return 0;
                }
                g_executed_lines++;
                if ((g_executed_lines % JOB_STATUS_LOG_PERIOD_LINES) == 0) {
                    osal_printk("[JOB_EXEC] line=%lu pos=%u queue=%u\r\n",
                                g_executed_lines, (unsigned int)pos,
                                (unsigned int)motion_executor_queue_depth());
                }
            }
            line_pos = 0;
        } else if (line_pos < (JOB_LINE_MAX - 1U)) {
            line[line_pos++] = ch;
        } else {
            job_manager_safe_stop("line-too-long");
            return 0;
        }
    }

    if (!g_abort_requested && line_pos > 0) {
        line[line_pos] = '\0';
        strip_line(line);
        if (line[0] != '\0') {
            if (!execute_line(line)) {
                job_manager_safe_stop("execute-final-line-fail");
                return 0;
            }
            g_executed_lines++;
        }
    }

    wait_motion_idle(MOTION_END_DRAIN_TIMEOUT_MS);
    laser_force_off();
    if (!g_abort_requested) {
        g_state = JOB_STATE_IDLE;
        osal_printk("[JOB_EXEC] done job=%u lines=%lu x=%.3f y=%.3f\r\n",
                    (unsigned int)job_cache_job_id(), g_executed_lines,
                    motion_executor_get_x(), motion_executor_get_y());
        job_cache_clear();
    }
    return 0;
}

static sle_job_status_t start_job_execution(uint32_t job_id)
{
    if (g_state != JOB_STATE_JOB_READY || !job_cache_is_ready() || job_id != job_cache_job_id()) {
        return JOB_STATUS_NOT_READY;
    }

    motion_executor_clear_abort();
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(job_exec_task, NULL, "job_exec", JOB_EXEC_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        return JOB_STATUS_INTERNAL_ERROR;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_JOB_EXECUTOR) != OSAL_SUCCESS) {
        osal_printk("[JOB_RX] set exec priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();
    return JOB_STATUS_OK;
}

static void handle_job_begin(const sle_packet_view_t *pkt)
{
    if (pkt->len != sizeof(job_begin_payload_t)) {
        send_ack(pkt->type, pkt->seq, JOB_STATUS_BAD_JOB);
        return;
    }
    job_begin_payload_t begin;
    memcpy(&begin, pkt->payload, sizeof(begin));
    sle_job_status_t st = job_cache_begin(begin.job_id, begin.total_size, begin.job_crc16);
    if (st == JOB_STATUS_OK) {
        motion_executor_clear_abort();
        g_state = JOB_STATE_RECEIVING_JOB;
        g_abort_requested = false;
        seq_commit(pkt->seq);
    }
    osal_printk("[JOB_BEGIN] seq=%u job=%u size=%u crc=0x%04x st=%u\r\n",
                pkt->seq, (unsigned int)begin.job_id, (unsigned int)begin.total_size,
                begin.job_crc16, st);
    send_ack(pkt->type, pkt->seq, st);
}

static void handle_job_data(const sle_packet_view_t *pkt)
{
    if (g_state != JOB_STATE_RECEIVING_JOB || pkt->len < sizeof(job_data_payload_t)) {
        send_ack(pkt->type, pkt->seq, JOB_STATUS_BAD_STATE);
        return;
    }
    job_data_payload_t hdr;
    memcpy(&hdr, pkt->payload, sizeof(hdr));
    if (hdr.data_len == 0 || pkt->len != (uint16_t)(sizeof(job_data_payload_t) + hdr.data_len)) {
        send_ack(pkt->type, pkt->seq, JOB_STATUS_BAD_JOB);
        return;
    }

    sle_job_status_t st = job_cache_write(hdr.job_id, hdr.offset,
                                          &pkt->payload[sizeof(job_data_payload_t)], hdr.data_len);
    if (st == JOB_STATUS_OK) {
        seq_commit(pkt->seq);
    }
    if ((g_packet_count <= 8UL) || ((g_packet_count % 32UL) == 0UL) || st != JOB_STATUS_OK) {
        osal_printk("[JOB_DATA] seq=%u job=%u off=%u len=%u rx=%u st=%u\r\n",
                    pkt->seq, (unsigned int)hdr.job_id, (unsigned int)hdr.offset, hdr.data_len,
                    (unsigned int)job_cache_received(), st);
    }
    send_ack(pkt->type, pkt->seq, st);
}

static void handle_job_end(const sle_packet_view_t *pkt)
{
    if (g_state != JOB_STATE_RECEIVING_JOB || pkt->len != sizeof(job_end_payload_t)) {
        send_ack(pkt->type, pkt->seq, JOB_STATUS_BAD_STATE);
        return;
    }
    job_end_payload_t end;
    memcpy(&end, pkt->payload, sizeof(end));
    sle_job_status_t st = job_cache_finish(end.job_id, end.total_size, end.job_crc16);
    if (st == JOB_STATUS_OK) {
        g_state = JOB_STATE_JOB_READY;
        seq_commit(pkt->seq);
    } else {
        g_state = JOB_STATE_ERROR;
    }
    osal_printk("[JOB_END] seq=%u job=%u size=%u crc=0x%04x st=%u state=%s\r\n",
                pkt->seq, (unsigned int)end.job_id, (unsigned int)end.total_size,
                end.job_crc16, st, state_name(g_state));
    send_ack(pkt->type, pkt->seq, st);
}

static void handle_exec_start(const sle_packet_view_t *pkt)
{
    if (pkt->len != sizeof(exec_start_payload_t)) {
        send_ack(pkt->type, pkt->seq, JOB_STATUS_BAD_JOB);
        return;
    }
    exec_start_payload_t start;
    memcpy(&start, pkt->payload, sizeof(start));
    sle_job_status_t st = start_job_execution(start.job_id);
    if (st == JOB_STATUS_OK) {
        seq_commit(pkt->seq);
    }
    osal_printk("[EXEC_START] seq=%u job=%u st=%u state=%s\r\n",
                pkt->seq, (unsigned int)start.job_id, st, state_name(g_state));
    send_ack(pkt->type, pkt->seq, st);
}

void job_manager_on_packet(const uint8_t *data, uint16_t len)
{
    sle_packet_view_t pkt;
    if (!sle_packet_decode(data, len, &pkt)) {
        osal_printk("[JOB_RX] bad packet len=%u\r\n", len);
        send_ack(0, 0, JOB_STATUS_BAD_CRC);
        return;
    }

    g_packet_count++;
    if (!seq_accepts(&pkt)) {
        osal_printk("[JOB_RX] seq error type=0x%02x got=%u expect=%u state=%s\r\n",
                    pkt.type, pkt.seq, g_expected_seq, state_name(g_state));
        send_ack(pkt.type, pkt.seq, JOB_STATUS_BAD_SEQ);
        return;
    }

    switch (pkt.type) {
        case PKT_JOB_BEGIN:
            handle_job_begin(&pkt);
            break;
        case PKT_JOB_DATA:
            handle_job_data(&pkt);
            break;
        case PKT_JOB_END:
            handle_job_end(&pkt);
            break;
        case PKT_EXEC_START:
            handle_exec_start(&pkt);
            break;
        case PKT_EXEC_STOP:
            job_manager_safe_stop("exec-stop");
            seq_commit(pkt.seq);
            send_ack(pkt.type, pkt.seq, JOB_STATUS_OK);
            break;
        case PKT_JOB_ABORT:
            job_manager_safe_stop("job-abort");
            job_cache_clear();
            g_state = JOB_STATE_IDLE;
            seq_commit(pkt.seq);
            send_ack(pkt.type, pkt.seq, JOB_STATUS_OK);
            break;
        case PKT_STATUS_REQ:
            if (pkt.seq == g_expected_seq) {
                seq_commit(pkt.seq);
            }
            send_status(JOB_STATUS_OK);
            break;
        default:
            osal_printk("[JOB_RX] unsupported type=0x%02x seq=%u\r\n", pkt.type, pkt.seq);
            send_ack(pkt.type, pkt.seq, JOB_STATUS_BAD_JOB);
            break;
    }
}

void job_manager_on_disconnect(void)
{
    job_manager_safe_stop("sle-disconnect");
    job_cache_clear();
    g_state = JOB_STATE_IDLE;
    g_expected_seq = 1;
    g_last_seq = 0;
}

void job_manager_init(void)
{
    job_cache_init();
    g_state = JOB_STATE_IDLE;
    g_abort_requested = false;
    g_expected_seq = 1;
    g_last_seq = 0;
    g_resp_seq = 1;
    g_executed_lines = 0;
    g_packet_count = 0;
    g_nack_count = 0;
    osal_printk("[JOB_RX] manager init cache=%u\r\n", (unsigned int)job_cache_size());
}
