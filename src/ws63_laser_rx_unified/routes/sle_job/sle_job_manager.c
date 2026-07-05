/**
 * @file job_manager.c
 * @brief Structured job protocol state machine with sliding-window support.
 */
#include "sle_job_manager.h"
#include "common_def.h"
#include "sle_job_config.h"
#include "sle_job_gcode_processor.h"
#include "sle_job_cache.h"
#include "laser_ctrl.h"
#include "sle_job_motion_executor.h"
#include "sle_job_packet.h"
#include "sle_job_protocol.h"
#include "route_manager.h"
#include "sle_errcode.h"
#include "sle_job_route_server.h"
#include "soc_osal.h"
#include "systick.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLE_JOB_LINE_MAX 160
#define SLE_JOB_EXEC_TASK_STACK_SIZE 0x2000
#define SLE_JOB_EXEC_STREAM_CACHE_LOW_WATERMARK 4096U
#define SLE_JOB_EXEC_STREAM_QUEUE_HIGH_WATERMARK 12U
#define SLE_JOB_EXEC_STREAM_THROTTLE_SLEEP_MS 5U
#define SLE_JOB_EXEC_STREAM_THROTTLE_LOG_MS 500U
#define SLE_JOB_ROUTE_SWITCH_DELAY_MS 200U
#define SLE_JOB_ROUTE_SWITCH_TASK_STACK_SIZE 0x1000U
#define SLE_JOB_PANEL_STATUS_PERIOD_MS 500U
#define SLE_JOB_PANEL_STATUS_TASK_STACK_SIZE 0x1000U
#define SLE_JOB_PANEL_STATUS_TASK_PRIO 6
#define SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE 0
#define SLE_JOB_SEND_SLOW_MS 50U

static volatile sle_job_state_t g_state = SLE_JOB_STATE_IDLE;
static volatile bool g_abort_requested = false;
static volatile bool g_pause_requested = false;
static volatile bool g_exec_active = false;
static volatile bool g_focus_active = false;
static volatile bool g_route_switch_pending = false;
static uint16_t g_expected_seq = 1;
static uint16_t g_last_seq = 0;
static uint16_t g_resp_seq = 1;
static uint32_t g_executed_lines = 0;
static uint32_t g_packet_count = 0;
static uint32_t g_nack_count = 0;
static uint32_t g_diag_rx_data_count = 0;

static uint8_t  g_last_ack_type = 0;
static uint16_t g_last_ack_ack_seq = 0;
static uint8_t  g_last_ack_status = 0;
static uint32_t g_last_ack_offset = 0;
static uint32_t g_last_ack_credit = 0;
static volatile bool g_panel_status_task_started = false;
#if SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE
static uint16_t g_panel_status_seq = 1;
#endif

static const char *state_name(sle_job_state_t state)
{
    switch (state) {
        case SLE_JOB_STATE_IDLE: return "IDLE";
        case SLE_JOB_STATE_RECEIVING_JOB: return "RECEIVING_JOB";
        case SLE_JOB_STATE_JOB_READY: return "JOB_READY";
        case SLE_JOB_STATE_EXECUTING: return "EXECUTING";
        case SLE_JOB_STATE_PAUSED: return "PAUSED";
        case SLE_JOB_STATE_ABORTED: return "ABORTED";
        case SLE_JOB_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static void focus_force_off(void)
{
    g_focus_active = false;
    laser_force_off();
}

static uint16_t next_resp_seq(void)
{
    uint16_t seq = g_resp_seq++;
    if (g_resp_seq == 0) {
        g_resp_seq = 1;
    }
    return seq;
}

static errcode_t send_packet(uint8_t type, const void *payload, uint16_t payload_len)
{
    uint8_t buf[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t out_len = 0;
    if (!sle_job_packet_encode(type, 0, next_resp_seq(), payload, payload_len,
                           buf, sizeof(buf), &out_len)) {
        osal_printk("[JOB_RX] encode response fail type=0x%02x\r\n", type);
        return ERRCODE_SLE_FAIL;
    }
    uint32_t t_send = (uint32_t)uapi_systick_get_ms();
    errcode_t ret = sle_job_route_server_send_packet(buf, out_len);
    uint32_t send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
    if (ret != ERRCODE_SLE_SUCCESS || send_ms >= SLE_JOB_SEND_SLOW_MS) {
        osal_printk("[RX_SEND_TIMING] type=0x%02x len=%u ret=0x%x send_ms=%u owner=%u conns=%u state=%s\r\n",
                    type, (unsigned int)out_len, (unsigned int)ret,
                    (unsigned int)send_ms,
                    (unsigned int)sle_job_route_server_get_owner_conn_id(),
                    (unsigned int)sle_job_route_server_get_connection_count(),
                    state_name(g_state));
    }
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[JOB_RX] send response fail type=0x%02x ret=0x%x\r\n", type, ret);
    }
    return ret;
}

#if SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE
static errcode_t broadcast_packet(uint8_t type, const void *payload, uint16_t payload_len)
{
    uint8_t buf[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t out_len = 0;
    if (!sle_job_packet_encode(type, 0, next_resp_seq(), payload, payload_len,
                           buf, sizeof(buf), &out_len)) {
        osal_printk("[JOB_RX] encode broadcast fail type=0x%02x\r\n", type);
        return ERRCODE_SLE_FAIL;
    }
    return sle_job_route_server_broadcast_packet(buf, out_len);
}

static void send_panel_status(void)
{
    sle_job_panel_status_payload_t st = {0};
    st.seq = g_panel_status_seq++;
    if (g_panel_status_seq == 0) {
        g_panel_status_seq = 1;
    }

    st.owner = (sle_job_route_server_get_owner_conn_id() == 0xFFFFU) ?
        SLE_JOB_PANEL_OWNER_NONE : SLE_JOB_PANEL_OWNER_HOST;
    if (g_state == SLE_JOB_STATE_ERROR || g_state == SLE_JOB_STATE_ABORTED) {
        st.mode = SLE_JOB_PANEL_MODE_ERROR;
    } else if (st.owner == SLE_JOB_PANEL_OWNER_HOST) {
        st.mode = SLE_JOB_PANEL_MODE_ONLINE;
    } else {
        st.mode = SLE_JOB_PANEL_MODE_IDLE;
    }
    st.job_state = (uint8_t)g_state;
    st.flags = 0;
    if (g_focus_active) {
        st.flags |= SLE_JOB_PANEL_FLAG_FOCUS_ACTIVE;
    }
    if (laser_is_enabled()) {
        st.flags |= SLE_JOB_PANEL_FLAG_LASER_ACTIVE;
    }
    if (sle_job_route_server_get_owner_conn_id() != 0xFFFFU) {
        st.flags |= SLE_JOB_PANEL_FLAG_OWNER_LINK;
    }
    if (sle_job_route_server_get_connection_count() > 0U) {
        st.flags |= SLE_JOB_PANEL_FLAG_ANY_LINK;
    }
    st.job_id = sle_job_cache_job_id();
    st.received_size = sle_job_cache_received();
    st.total_size = sle_job_cache_total_size();
    st.executed_lines = (uint32_t)g_executed_lines;
    st.cache_free = sle_job_cache_free();
    st.last_error = (g_state == SLE_JOB_STATE_ERROR) ? 1U : 0U;
    st.tick_ms = (uint32_t)uapi_systick_get_ms();

    (void)sle_job_route_server_update_panel_status_adv(&st);

    errcode_t ret = broadcast_packet(SLE_JOB_PKT_PANEL_STATUS, &st, sizeof(st));
    if (ret != ERRCODE_SLE_SUCCESS && sle_job_route_server_get_connection_count() > 0U) {
        static uint32_t s_panel_bcast_fail_count = 0;
        if ((s_panel_bcast_fail_count++ & 0x0FU) == 0U) {
            osal_printk("[PANEL_STATUS] broadcast ret=0x%x conns=%u owner=%u\r\n",
                        ret, sle_job_route_server_get_connection_count(),
                        sle_job_route_server_get_owner_conn_id());
        }
    }
}

static int panel_status_task(void *arg)
{
    unused(arg);
    while (1) {
        send_panel_status();
        osal_msleep(SLE_JOB_PANEL_STATUS_PERIOD_MS);
    }
    return 0;
}
#endif

static void start_panel_status_task_once(void)
{
#if !SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE
    if (!g_panel_status_task_started) {
        g_panel_status_task_started = true;
        osal_printk("[PANEL_STATUS] disabled: adv_update=0 broadcast=0\r\n");
    }
    return;
#else
    if (g_panel_status_task_started) {
        return;
    }
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(panel_status_task, NULL, "panel_stat",
                                          SLE_JOB_PANEL_STATUS_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[PANEL_STATUS] create task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_PANEL_STATUS_TASK_PRIO) != OSAL_SUCCESS) {
        osal_printk("[PANEL_STATUS] set priority failed\r\n");
    }
    g_panel_status_task_started = true;
    osal_kfree(task);
    osal_kthread_unlock();
#endif
}

static void send_ack_with_reason(uint8_t ack_type, uint16_t ack_seq, sle_job_status_t status, const char *reason)
{
    sle_job_ack_payload_t ack = {0};
    ack.ack_type = ack_type;
    ack.status = (uint8_t)status;
    ack.ack_seq = ack_seq;
    ack.job_id = sle_job_cache_job_id();
    ack.offset = sle_job_cache_received();
    ack.credit = sle_job_cache_free();
    g_last_ack_type = ack_type;
    g_last_ack_ack_seq = ack_seq;
    g_last_ack_status = status;
    g_last_ack_offset = ack.offset;
    g_last_ack_credit = ack.credit;
    uint32_t t_send = (uint32_t)uapi_systick_get_ms();
    errcode_t send_ret = send_packet((status == SLE_JOB_STATUS_OK) ? SLE_JOB_PKT_ACK : SLE_JOB_PKT_NACK, &ack, sizeof(ack));
    uint32_t ack_send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
    if (ack_type != SLE_JOB_PKT_JOB_DATA ||
        (SLE_JOB_DIAG_LOG && (g_diag_rx_data_count <= 8U || status != SLE_JOB_STATUS_OK ||
                              send_ret != ERRCODE_SLE_SUCCESS)) ||
        send_ret != ERRCODE_SLE_SUCCESS ||
        ack_send_ms >= SLE_JOB_SEND_SLOW_MS) {
        osal_printk("[RX_ACK] t=%u type=0x%02x seq=%u st=%u off=%u credit=%u ret=0x%x send_ms=%u state=%s\r\n",
                    (unsigned int)uapi_systick_get_ms(), (unsigned int)ack_type,
                    (unsigned int)ack_seq, (unsigned int)status,
                    (unsigned int)ack.offset, (unsigned int)ack.credit,
                    (unsigned int)send_ret, (unsigned int)ack_send_ms,
                    state_name(g_state));
    }
    if (status != SLE_JOB_STATUS_OK) {
        osal_printk("[JOB_RX_NACK_SEND] type=%u seq=%u status=%u state=%s reason=%s\r\n",
                    (unsigned int)ack_type, (unsigned int)ack_seq, (unsigned int)status,
                    state_name(g_state), (reason != NULL) ? reason : "unspecified");
        g_nack_count++;
    }
}

static void send_ack(uint8_t ack_type, uint16_t ack_seq, sle_job_status_t status)
{
    send_ack_with_reason(ack_type, ack_seq, status, NULL);
}

static void send_status(sle_job_status_t status)
{
    sle_job_status_resp_payload_t resp = {0};
    resp.state = (uint8_t)g_state;
    resp.status = (uint8_t)status;
    resp.last_seq = g_last_seq;
    resp.job_id = sle_job_cache_job_id();
    resp.received_size = sle_job_cache_received();
    resp.total_size = sle_job_cache_total_size();
    resp.cache_free = sle_job_cache_free();
    resp.executed_lines = (uint32_t)g_executed_lines;
    (void)send_packet(SLE_JOB_PKT_STATUS_RESP, &resp, sizeof(resp));
}

static bool seq_accepts(const sle_job_packet_view_t *pkt)
{
    if (pkt->type == SLE_JOB_PKT_EXEC_STOP || pkt->type == SLE_JOB_PKT_JOB_ABORT || pkt->type == SLE_JOB_PKT_STATUS_REQ) {
        return true;
    }
    if (pkt->type == SLE_JOB_PKT_JOB_BEGIN) {
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

static bool handle_replayed_packet(const sle_job_packet_view_t *pkt)
{
    if (g_last_seq != 0 && pkt->seq == g_last_seq) {
        osal_printk("[JOB_RX_SEQ] seq=%u expected=%u duplicate=1 cached_type=%u cached_status=%u cached_off=%u cached_credit=%u\r\n",
                    (unsigned int)pkt->seq, (unsigned int)g_expected_seq,
                    (unsigned int)g_last_ack_type, (unsigned int)g_last_ack_status,
                    (unsigned int)g_last_ack_offset, (unsigned int)g_last_ack_credit);
        send_ack(g_last_ack_type, g_last_ack_ack_seq, g_last_ack_status);
        return true;
    }
    if (pkt->seq < g_last_seq) {
        static uint32_t s_drop_count = 0;
        if ((s_drop_count++ & 0x1F) == 0) {
            osal_printk("[JOB_RX] drop old seq=%u last=%u expect=%u\r\n",
                        (unsigned int)pkt->seq, (unsigned int)g_last_seq,
                        (unsigned int)g_expected_seq);
        }
        return true;
    }
    return false;
}

static void wait_motion_idle(uint32_t timeout_ms)
{
    unsigned long start = (unsigned long)uapi_systick_get_ms();
    while (sle_job_motion_executor_is_busy()) {
        if (((unsigned long)uapi_systick_get_ms() - start) >= timeout_ms) {
            break;
        }
        osal_msleep(1);
    }
}

static void wait_exec_inactive(uint32_t timeout_ms)
{
    unsigned long start = (unsigned long)uapi_systick_get_ms();
    while (g_exec_active) {
        if (((unsigned long)uapi_systick_get_ms() - start) >= timeout_ms) {
            break;
        }
        osal_msleep(1);
    }
}

void sle_job_manager_safe_stop(const char *reason)
{
    osal_printk("[JOB_SAFE_STOP] reason=%s state=%s\r\n",
                (reason != NULL) ? reason : "unknown", state_name(g_state));
    focus_force_off();
    g_abort_requested = true;
    g_pause_requested = false;
    sle_job_motion_executor_request_abort();
    sle_job_motion_executor_flush();
    sle_job_motion_cmd_t cmd;
    sle_job_gcode_processor_build_emergency_stop(&cmd);
    sle_job_motion_executor_execute(&cmd);
    laser_force_off();
    g_state = SLE_JOB_STATE_ABORTED;
    g_exec_active = false;
}

static void pause_job_execution(const char *reason)
{
    osal_printk("[JOB_PAUSE] reason=%s state=%s\r\n",
                (reason != NULL) ? reason : "unknown", state_name(g_state));
    focus_force_off();
    g_pause_requested = true;
    sle_job_motion_executor_request_abort();
    sle_job_motion_executor_flush();
    laser_force_off();
    if (g_state == SLE_JOB_STATE_EXECUTING || g_state == SLE_JOB_STATE_RECEIVING_JOB ||
        g_state == SLE_JOB_STATE_JOB_READY) {
        g_state = SLE_JOB_STATE_PAUSED;
    }
    wait_exec_inactive(200U);
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
    sle_job_motion_cmd_t cmds[4];
    int cmd_count = 0;
    bool drain_and_off = line_contains_mcode(line, 5);

    if (!sle_job_gcode_process_line(line, (int)strlen(line), cmds, 4, &cmd_count)) {
        osal_printk("[RX_GCODE_ERR] line=%u reason=parse_failed raw=\"%.80s\"\r\n",
                    (unsigned int)(g_executed_lines + 1U), line);
        return false;
    }
    if (SLE_JOB_DIAG_LOG && cmd_count > 0) {
        osal_printk("[JOB_EXEC] line=%u cmd_count=%d cmd0=%d\r\n",
                    (unsigned int)(g_executed_lines + 1U), cmd_count, cmds[0].cmd);
    }
    for (int i = 0; i < cmd_count; i++) {
        while (sle_job_motion_executor_queue_depth() >= SLE_JOB_MOTION_QUEUE_OK_WATERMARK &&
               !g_abort_requested && !g_pause_requested) {
            osal_msleep(1);
        }
        if (g_abort_requested || g_pause_requested) {
            osal_printk("[JOB_EXEC] %s requested during enqueue line=%u\r\n",
                        g_pause_requested ? "pause" : "abort",
                        (unsigned int)(g_executed_lines + 1U));
            return false;
        }
        if (!sle_job_motion_executor_enqueue(&cmds[i])) {
            osal_printk("[JOB_EXEC] enqueue fail line=%u cmd=%d\r\n",
                        (unsigned int)(g_executed_lines + 1U), cmds[i].cmd);
            return false;
        }
    }
    if (drain_and_off) {
        if (SLE_JOB_DIAG_LOG) {
            osal_printk("[JOB_EXEC] M5 detected, draining motion queue\r\n");
        }
        wait_motion_idle(SLE_JOB_MOTION_END_DRAIN_TIMEOUT_MS);
        laser_force_off();
    }
    return true;
}

static void throttle_streaming_executor(void)
{
    if (sle_job_cache_is_all_received()) {
        return;
    }

    uint32_t avail = sle_job_cache_available();
    uint16_t queue = sle_job_motion_executor_queue_depth();
    if (avail >= SLE_JOB_EXEC_STREAM_CACHE_LOW_WATERMARK &&
        queue < SLE_JOB_EXEC_STREAM_QUEUE_HIGH_WATERMARK) {
        return;
    }

    osal_msleep(SLE_JOB_EXEC_STREAM_THROTTLE_SLEEP_MS);
}

static int job_exec_task(void *arg)
{
    unused(arg);
    char line[SLE_JOB_LINE_MAX];
    uint16_t line_pos = 0;

    if (g_abort_requested || g_pause_requested) {
        osal_printk("[JOB_EXEC] %s already requested, exiting\r\n",
                    g_pause_requested ? "pause" : "abort");
        g_exec_active = false;
        return 0;
    }

    uint32_t wait_start_ms = 0;

    while (!g_abort_requested && !g_pause_requested) {
        int ch = sle_job_cache_read_byte();
        if (ch < 0) {
            if (sle_job_cache_is_all_received()) {
                break;
            }
            if (wait_start_ms == 0) {
                wait_start_ms = (uint32_t)uapi_systick_get_ms();
            }
            uint32_t wait_elapsed = (uint32_t)uapi_systick_get_ms() - wait_start_ms;
            if (wait_elapsed >= SLE_JOB_EXEC_WAIT_DATA_TIMEOUT_MS) {
                osal_printk("[RX_WAIT_TIMEOUT] consumed=%u available=%u total=%u all_received=%d state=%s\r\n",
                            (unsigned int)sle_job_cache_received(),
                            (unsigned int)sle_job_cache_available(),
                            (unsigned int)sle_job_cache_total_size(),
                            (int)sle_job_cache_is_all_received(),
                            state_name(g_state));
                sle_job_manager_safe_stop("exec-wait-timeout");
                g_exec_active = false;
                return 0;
            }
            osal_msleep(1);
            continue;
        }
        wait_start_ms = 0;

        if (ch == '\r' || ch == '\n') {
            if (line_pos == 0) {
                continue;
            }
            line[line_pos] = '\0';
            strip_line(line);
            if (line[0] != '\0') {
                if (!execute_line(line)) {
                    if (g_pause_requested) {
                        laser_force_off();
                        g_exec_active = false;
                        osal_printk("[JOB_EXEC] paused job=%u lines=%u consumed=%u received=%u\r\n",
                                    (unsigned int)sle_job_cache_job_id(),
                                    (unsigned int)g_executed_lines,
                                    (unsigned int)sle_job_cache_consumed(),
                                    (unsigned int)sle_job_cache_received());
                        return 0;
                    }
                    sle_job_manager_safe_stop("execute-line-fail");
                    g_exec_active = false;
                    return 0;
                }
                g_executed_lines++;
                throttle_streaming_executor();
            }
            line_pos = 0;
        } else if (line_pos < (SLE_JOB_LINE_MAX - 1U)) {
            line[line_pos++] = (char)ch;
        } else {
            sle_job_manager_safe_stop("line-too-long");
            g_exec_active = false;
            return 0;
        }
    }

    if (!g_abort_requested && !g_pause_requested && line_pos > 0) {
        line[line_pos] = '\0';
        strip_line(line);
        if (line[0] != '\0') {
            if (!execute_line(line)) {
                if (g_pause_requested) {
                    laser_force_off();
                    g_exec_active = false;
                    osal_printk("[JOB_EXEC] paused job=%u lines=%u consumed=%u received=%u\r\n",
                                (unsigned int)sle_job_cache_job_id(),
                                (unsigned int)g_executed_lines,
                                (unsigned int)sle_job_cache_consumed(),
                                (unsigned int)sle_job_cache_received());
                    return 0;
                }
                sle_job_manager_safe_stop("execute-final-line-fail");
                g_exec_active = false;
                return 0;
            }
            g_executed_lines++;
            throttle_streaming_executor();
        }
    }

    if (g_pause_requested) {
        laser_force_off();
        g_exec_active = false;
        osal_printk("[JOB_EXEC] paused job=%u lines=%u consumed=%u received=%u\r\n",
                    (unsigned int)sle_job_cache_job_id(),
                    (unsigned int)g_executed_lines,
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_received());
        return 0;
    }

    wait_motion_idle(SLE_JOB_MOTION_END_DRAIN_TIMEOUT_MS);
    laser_force_off();
    focus_force_off();
    if (!g_abort_requested) {
        g_state = SLE_JOB_STATE_IDLE;
        int32_t x_um = (int32_t)(sle_job_motion_executor_get_x() * 1000.0);
        int32_t y_um = (int32_t)(sle_job_motion_executor_get_y() * 1000.0);
        osal_printk("[JOB_EXEC] done job=%u lines=%u x_um=%d y_um=%d "
                    "seg=%lu short=%lu late=%lu missed=%lu max_late_us=%lu "
                    "q=%u min_mark_us=%u sample_us=%u profile=%u\r\n",
                    (unsigned int)sle_job_cache_job_id(), (unsigned int)g_executed_lines,
                    (int)x_um, (int)y_um,
                    sle_job_motion_executor_motion_segment_count(),
                    sle_job_motion_executor_short_segment_count(),
                    sle_job_motion_executor_late_sample_count(),
                    sle_job_motion_executor_missed_sample_count(),
                    sle_job_motion_executor_max_sample_late_us(),
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    (unsigned int)SLE_JOB_MOTION_MIN_MARK_SEGMENT_US,
                    (unsigned int)SLE_JOB_MOTION_SAMPLE_PERIOD_US,
                    (unsigned int)SLE_JOB_MOTION_SPEED_PROFILE);
        sle_job_cache_clear();
    }
    g_exec_active = false;
    return 0;
}

static sle_job_status_t validate_job_execution(uint32_t job_id)
{
    if (g_exec_active) {
        return SLE_JOB_STATUS_BAD_STATE;
    }
    if (g_state == SLE_JOB_STATE_JOB_READY || g_state == SLE_JOB_STATE_RECEIVING_JOB) {
        if (sle_job_cache_job_id() == job_id && sle_job_cache_received() > 0) {
            return SLE_JOB_STATUS_OK;
        }
        return SLE_JOB_STATUS_BAD_JOB;
    }
    return SLE_JOB_STATUS_NOT_READY;
}

static sle_job_status_t validate_job_resume(void)
{
    if (g_exec_active) {
        return SLE_JOB_STATUS_BAD_STATE;
    }
    if (g_state != SLE_JOB_STATE_PAUSED) {
        return SLE_JOB_STATUS_BAD_STATE;
    }
    if (sle_job_cache_job_id() == 0 || sle_job_cache_received() == 0) {
        return SLE_JOB_STATUS_BAD_JOB;
    }
    return SLE_JOB_STATUS_OK;
}

static sle_job_status_t launch_job_execution(void)
{
    sle_job_motion_executor_clear_abort();
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(job_exec_task, NULL, "job_exec", SLE_JOB_EXEC_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        return SLE_JOB_STATUS_INTERNAL_ERROR;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_TASK_PRIO_JOB_EXECUTOR) != OSAL_SUCCESS) {
        osal_printk("[JOB_RX] set exec priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();
    return SLE_JOB_STATUS_OK;
}

static void handle_job_begin(const sle_job_packet_view_t *pkt)
{
    if (pkt->len != sizeof(sle_job_begin_payload_t)) {
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
        return;
    }

    if (g_exec_active || g_state == SLE_JOB_STATE_EXECUTING) {
        osal_printk("[JOB_BEGIN] reject: still executing state=%s\r\n", state_name(g_state));
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_STATE);
        return;
    }

    sle_job_begin_payload_t begin;
    memcpy(&begin, pkt->payload, sizeof(begin));
    sle_job_status_t st = sle_job_cache_begin(begin.job_id, begin.total_size, begin.job_crc16);
    if (st == SLE_JOB_STATUS_OK) {
        sle_job_motion_executor_clear_abort();
        g_state = SLE_JOB_STATE_RECEIVING_JOB;
        g_abort_requested = false;
        g_pause_requested = false;
        g_executed_lines = 0;
        g_diag_rx_data_count = 0;
        seq_commit(pkt->seq);
    }
    send_ack(pkt->type, pkt->seq, st);
}

static bool rx_should_log_data_timing(uint32_t data_index, uint32_t total_ms)
{
#if SLE_JOB_TIMING_LOG
    return data_index <= SLE_JOB_TIMING_FIRST_PACKETS ||
           (SLE_JOB_TIMING_EVERY_PACKETS > 0U && (data_index % SLE_JOB_TIMING_EVERY_PACKETS) == 0U) ||
           total_ms >= SLE_JOB_TIMING_SLOW_MS;
#else
    unused(data_index);
    unused(total_ms);
    return false;
#endif
}

static void handle_job_data(const sle_job_packet_view_t *pkt)
{
    uint32_t t_start = (uint32_t)uapi_systick_get_ms();

    if (SLE_JOB_DIAG_LOG) {
        osal_printk("[JOB_DATA_IN] state=%s seq=%u pkt_len=%u payload=%p\r\n",
                    state_name(g_state), (unsigned int)pkt->seq,
                    (unsigned int)pkt->len, (const void *)pkt->payload);
    }

    if (g_state != SLE_JOB_STATE_RECEIVING_JOB && g_state != SLE_JOB_STATE_EXECUTING &&
        g_state != SLE_JOB_STATE_PAUSED) {
        osal_printk("[JOB_DATA_REJECT] reason=bad_state state=%s seq=%u pkt_len=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq,
                    (unsigned int)pkt->len);
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_STATE, "bad_state");
        return;
    }
    if (pkt->len < sizeof(sle_job_data_payload_t)) {
        osal_printk("[JOB_DATA_REJECT] reason=short_packet state=%s seq=%u pkt_len=%u need=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq,
                    (unsigned int)pkt->len, (unsigned int)sizeof(sle_job_data_payload_t));
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB, "short_packet");
        return;
    }
    sle_job_data_payload_t hdr;
    memcpy(&hdr, pkt->payload, sizeof(hdr));
    if (SLE_JOB_DIAG_LOG) {
        osal_printk("[JOB_DATA_HDR] seq=%u job=%u off=%u data_len=%u pkt_len=%u expect_len=%u cache_rx=%u state=%s\r\n",
                    (unsigned int)pkt->seq, (unsigned int)hdr.job_id, (unsigned int)hdr.offset,
                    (unsigned int)hdr.data_len, (unsigned int)pkt->len,
                    (unsigned int)(sizeof(sle_job_data_payload_t) + hdr.data_len),
                    (unsigned int)sle_job_cache_received(), state_name(g_state));
    }

    if (hdr.data_len == 0) {
        osal_printk("[JOB_DATA_REJECT] reason=zero_len state=%s seq=%u job=%u off=%u pkt_len=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq,
                    (unsigned int)hdr.job_id, (unsigned int)hdr.offset, (unsigned int)pkt->len);
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB, "zero_len");
        return;
    }
    if (pkt->len != (uint16_t)(sizeof(sle_job_data_payload_t) + hdr.data_len)) {
        osal_printk("[JOB_DATA_REJECT] reason=len_mismatch state=%s seq=%u job=%u off=%u data_len=%u pkt_len=%u expect_len=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq,
                    (unsigned int)hdr.job_id, (unsigned int)hdr.offset,
                    (unsigned int)hdr.data_len, (unsigned int)pkt->len,
                    (unsigned int)(sizeof(sle_job_data_payload_t) + hdr.data_len));
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB, "len_mismatch");
        return;
    }

    uint32_t t_write = (uint32_t)uapi_systick_get_ms();
    sle_job_status_t st = sle_job_cache_write(hdr.job_id, hdr.offset,
                                          &pkt->payload[sizeof(sle_job_data_payload_t)], hdr.data_len);
    uint32_t write_ms = (uint32_t)uapi_systick_get_ms() - t_write;
    if (st == SLE_JOB_STATUS_OK) {
        seq_commit(pkt->seq);
    }
    g_diag_rx_data_count++;
    uint32_t data_index = g_diag_rx_data_count;
    if ((SLE_JOB_DIAG_LOG && g_diag_rx_data_count <= SLE_JOB_DIAG_LOG_MAX_DATA) ||
        st != SLE_JOB_STATUS_OK) {
        osal_printk("[JOB_DATA_RESULT] state=%s seq=%u job=%u off=%u len=%u cache_rx=%u st=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq,
                    (unsigned int)hdr.job_id, (unsigned int)hdr.offset,
                    (unsigned int)hdr.data_len, (unsigned int)sle_job_cache_received(), st);
    }
    uint32_t t_ack = (uint32_t)uapi_systick_get_ms();
    send_ack_with_reason(pkt->type, pkt->seq, st, (st == SLE_JOB_STATUS_OK) ? NULL : "cache_write");
    uint32_t ack_ms = (uint32_t)uapi_systick_get_ms() - t_ack;
    uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_start;
    if (rx_should_log_data_timing(data_index, total_ms) || st != SLE_JOB_STATUS_OK) {
        osal_printk("[RX_TIMING] seq=%u data_idx=%u off=%u len=%u write_ms=%u ack_ms=%u "
                    "total_ms=%u st=%u free=%u state=%s\r\n",
                    (unsigned int)pkt->seq, (unsigned int)data_index,
                    (unsigned int)hdr.offset, (unsigned int)hdr.data_len,
                    (unsigned int)write_ms, (unsigned int)ack_ms,
                    (unsigned int)total_ms, (unsigned int)st,
                    (unsigned int)sle_job_cache_free(), state_name(g_state));
    }
}

static void handle_job_end(const sle_job_packet_view_t *pkt)
{
    if ((g_state != SLE_JOB_STATE_RECEIVING_JOB && g_state != SLE_JOB_STATE_EXECUTING &&
         g_state != SLE_JOB_STATE_PAUSED) ||
        pkt->len != sizeof(sle_job_end_payload_t)) {
        osal_printk("[JOB_END_REJECT] state=%s seq=%u pkt_len=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq, (unsigned int)pkt->len);
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_STATE);
        return;
    }
    sle_job_end_payload_t end;
    memcpy(&end, pkt->payload, sizeof(end));
    sle_job_status_t st = sle_job_cache_finish(end.job_id, end.total_size, end.job_crc16);
    if (st == SLE_JOB_STATUS_OK) {
        if (g_state == SLE_JOB_STATE_RECEIVING_JOB) {
            g_state = SLE_JOB_STATE_JOB_READY;
        }
        sle_job_cache_set_all_received();
        seq_commit(pkt->seq);
    } else {
        if (g_state == SLE_JOB_STATE_RECEIVING_JOB) {
            g_state = SLE_JOB_STATE_ERROR;
        }
        sle_job_cache_set_all_received();
        g_abort_requested = true;
        g_pause_requested = false;
    }
    send_ack(pkt->type, pkt->seq, st);
}

static void handle_exec_start(const sle_job_packet_view_t *pkt)
{
    focus_force_off();
    if (pkt->len != sizeof(sle_job_exec_start_payload_t)) {
        osal_printk("[EXEC_START] bad pkt_len=%u need=%u\r\n",
                    (unsigned int)pkt->len, (unsigned int)sizeof(sle_job_exec_start_payload_t));
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
        return;
    }
    sle_job_exec_start_payload_t start;
    memcpy(&start, pkt->payload, sizeof(start));
    sle_job_status_t st = validate_job_execution(start.job_id);
    if (st == SLE_JOB_STATUS_OK) {
        g_exec_active = true;
        g_abort_requested = false;
        g_pause_requested = false;
        g_state = SLE_JOB_STATE_EXECUTING;
        seq_commit(pkt->seq);
    }
    send_ack(pkt->type, pkt->seq, st);
    if (st == SLE_JOB_STATUS_OK) {
        sle_job_status_t launch_st = launch_job_execution();
        if (launch_st != SLE_JOB_STATUS_OK) {
            osal_printk("[EXEC_START] launch failed st=%u\r\n", launch_st);
            sle_job_manager_safe_stop("exec-launch-fail");
        }
    } else {
        osal_printk("[EXEC_START] validate failed st=%u\r\n", st);
    }
}

static void handle_exec_resume(const sle_job_packet_view_t *pkt)
{
    focus_force_off();
    if (pkt->len != 0) {
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
        return;
    }

    sle_job_status_t st = validate_job_resume();
    if (st == SLE_JOB_STATUS_OK) {
        g_exec_active = true;
        g_abort_requested = false;
        g_pause_requested = false;
        sle_job_motion_executor_clear_abort();
        g_state = SLE_JOB_STATE_EXECUTING;
        seq_commit(pkt->seq);
    }
    send_ack(pkt->type, pkt->seq, st);
    if (st == SLE_JOB_STATUS_OK) {
        sle_job_status_t launch_st = launch_job_execution();
        if (launch_st != SLE_JOB_STATUS_OK) {
            osal_printk("[EXEC_RESUME] launch failed st=%u\r\n", launch_st);
            sle_job_manager_safe_stop("resume-launch-fail");
        } else {
            osal_printk("[EXEC_RESUME] resumed job=%u consumed=%u received=%u total=%u\r\n",
                        (unsigned int)sle_job_cache_job_id(),
                        (unsigned int)sle_job_cache_consumed(),
                        (unsigned int)sle_job_cache_received(),
                        (unsigned int)sle_job_cache_total_size());
        }
    } else {
        osal_printk("[EXEC_RESUME] validate failed st=%u state=%s\r\n", st, state_name(g_state));
    }
}

static void handle_focus_ctrl(const sle_job_packet_view_t *pkt)
{
    if (pkt->len != sizeof(sle_job_focus_ctrl_payload_t)) {
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
        return;
    }
    sle_job_focus_ctrl_payload_t fp;
    memcpy(&fp, pkt->payload, sizeof(fp));

    if (fp.on) {
        if (g_state != SLE_JOB_STATE_IDLE) {
            osal_printk("[FOCUS] reject on state=%s\r\n", state_name(g_state));
            send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_STATE);
            return;
        }
        if (fp.power > 100) {
            osal_printk("[FOCUS] reject bad_power=%u\r\n", (unsigned int)fp.power);
            send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
            return;
        }
        uint16_t internal_power = (uint16_t)(fp.power * 10U);
        laser_force_off();
        laser_set_power(internal_power);
        laser_enable(true);
        g_focus_active = true;
        seq_commit(pkt->seq);
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_OK);
    } else {
        focus_force_off();
        seq_commit(pkt->seq);
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_OK);
    }
}

static int route_switch_task(void *arg)
{
    unused(arg);

    osal_msleep(SLE_JOB_ROUTE_SWITCH_DELAY_MS);
    bool ok = route_manager_request_safe_switch(RX_ROUTE_LEGACY_WIFI);
    if (!ok) {
        osal_printk("[ROUTE_SWITCH] delayed switch failed target=LEGACY_WIFI\r\n");
        laser_force_off();
    }
    g_route_switch_pending = false;
    return ok ? 0 : -1;
}

static bool start_route_switch_task(void)
{
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(route_switch_task, NULL, "route_switch",
                                          SLE_JOB_ROUTE_SWITCH_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        return false;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_TASK_PRIO_JOB_EXECUTOR) != OSAL_SUCCESS) {
        osal_printk("[ROUTE_SWITCH] set switch priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();
    return true;
}

static void handle_route_switch(const sle_job_packet_view_t *pkt)
{
    if (pkt->len != sizeof(sle_job_route_switch_payload_t)) {
        osal_printk("[ROUTE_SWITCH] reject reason=bad_len len=%u need=%u\r\n",
                    (unsigned int)pkt->len,
                    (unsigned int)sizeof(sle_job_route_switch_payload_t));
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB, "bad_len");
        return;
    }

    sle_job_route_switch_payload_t req;
    memcpy(&req, pkt->payload, sizeof(req));
    if (req.target_route != SLE_JOB_ROUTE_TARGET_LEGACY_WIFI || req.flags != 0 || req.reserved != 0) {
        osal_printk("[ROUTE_SWITCH] reject reason=bad_target target=%u flags=%u reserved=%u\r\n",
                    (unsigned int)req.target_route, (unsigned int)req.flags,
                    (unsigned int)req.reserved);
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB, "bad_target");
        return;
    }

    if (g_route_switch_pending || !route_manager_can_request_switch(RX_ROUTE_LEGACY_WIFI)) {
        osal_printk("[ROUTE_SWITCH] reject reason=busy state=%s exec=%d queue=%u motion_busy=%d laser=%d switching=%d\r\n",
                    state_name(g_state), g_exec_active ? 1 : 0,
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    sle_job_motion_executor_is_busy() ? 1 : 0,
                    laser_is_enabled() ? 1 : 0,
                    route_manager_is_switching() ? 1 : 0);
        laser_force_off();
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_STATE, "busy");
        return;
    }

    laser_force_off();
    g_route_switch_pending = true;
    if (!start_route_switch_task()) {
        g_route_switch_pending = false;
        osal_printk("[ROUTE_SWITCH] reject reason=create_task_failed\r\n");
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_INTERNAL_ERROR, "create_task_failed");
        return;
    }

    seq_commit(pkt->seq);
    send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_OK);
}

static bool handle_control_fast_path(const sle_job_packet_view_t *pkt)
{
    switch (pkt->type) {
        case SLE_JOB_PKT_EXEC_STOP:
            osal_printk("[JOB_CTRL_FAST] type=EXEC_STOP seq=%u state=%s\r\n",
                        (unsigned int)pkt->seq, state_name(g_state));
            pause_job_execution("exec-stop");
            seq_commit(pkt->seq);
            send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_OK);
            return true;
        case SLE_JOB_PKT_EXEC_RESUME:
            osal_printk("[JOB_CTRL_FAST] type=EXEC_RESUME seq=%u state=%s\r\n",
                        (unsigned int)pkt->seq, state_name(g_state));
            handle_exec_resume(pkt);
            return true;
        case SLE_JOB_PKT_JOB_ABORT:
            osal_printk("[JOB_CTRL_FAST] type=JOB_ABORT seq=%u state=%s\r\n",
                        (unsigned int)pkt->seq, state_name(g_state));
            sle_job_manager_safe_stop("job-abort");
            sle_job_cache_clear();
            g_state = SLE_JOB_STATE_IDLE;
            seq_commit(pkt->seq);
            send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_OK);
            return true;
        case SLE_JOB_PKT_FOCUS_CTRL:
            osal_printk("[JOB_CTRL_FAST] type=FOCUS_CTRL seq=%u state=%s\r\n",
                        (unsigned int)pkt->seq, state_name(g_state));
            handle_focus_ctrl(pkt);
            return true;
        default:
            return false;
    }
}

void sle_job_manager_on_packet(uint16_t conn_id, const uint8_t *data, uint16_t len)
{
    unused(conn_id);
    sle_job_packet_view_t pkt;
    if (!sle_job_packet_decode(data, len, &pkt)) {
        osal_printk("[JOB_RX] bad packet len=%u\r\n", len);
        send_ack(0, g_expected_seq, SLE_JOB_STATUS_BAD_CRC);
        return;
    }

    g_packet_count++;
    if (handle_replayed_packet(&pkt)) {
        return;
    }
    if (handle_control_fast_path(&pkt)) {
        return;
    }

    if (!seq_accepts(&pkt)) {
        osal_printk("[JOB_RX_SEQ] seq=%u expected=%u future_seq=1\r\n",
                    (unsigned int)pkt.seq, (unsigned int)g_expected_seq);
        send_ack(pkt.type, pkt.seq, SLE_JOB_STATUS_BAD_SEQ);
        return;
    }

    switch (pkt.type) {
        case SLE_JOB_PKT_JOB_BEGIN:
            handle_job_begin(&pkt);
            break;
        case SLE_JOB_PKT_JOB_DATA:
            handle_job_data(&pkt);
            break;
        case SLE_JOB_PKT_JOB_END:
            handle_job_end(&pkt);
            break;
        case SLE_JOB_PKT_EXEC_START:
            handle_exec_start(&pkt);
            break;
        case SLE_JOB_PKT_EXEC_RESUME:
            handle_exec_resume(&pkt);
            break;
        case SLE_JOB_PKT_STATUS_REQ:
            if (pkt.seq == g_expected_seq) {
                seq_commit(pkt.seq);
            }
            send_status(SLE_JOB_STATUS_OK);
            break;
        case SLE_JOB_PKT_ROUTE_SWITCH:
            handle_route_switch(&pkt);
            break;
        default:
            osal_printk("[JOB_RX] unsupported type=0x%02x seq=%u\r\n", pkt.type, pkt.seq);
            send_ack(pkt.type, pkt.seq, SLE_JOB_STATUS_BAD_JOB);
            break;
    }
}

void sle_job_manager_on_disconnect(void)
{
    sle_job_manager_safe_stop("sle-disconnect");
    sle_job_cache_clear();
    g_state = SLE_JOB_STATE_IDLE;
    g_exec_active = false;
    g_pause_requested = false;
    g_focus_active = false;
    g_expected_seq = 1;
    g_last_seq = 0;
    g_last_ack_type = 0;
    g_last_ack_ack_seq = 0;
    g_last_ack_status = 0;
    g_last_ack_offset = 0;
    g_last_ack_credit = 0;
    g_route_switch_pending = false;
}

bool sle_job_manager_is_idle(void)
{
    return g_state == SLE_JOB_STATE_IDLE && !g_exec_active;
}

void sle_job_manager_init(void)
{
    sle_job_cache_init();
    g_state = SLE_JOB_STATE_IDLE;
    g_abort_requested = false;
    g_pause_requested = false;
    g_exec_active = false;
    g_focus_active = false;
    g_expected_seq = 1;
    g_last_seq = 0;
    g_resp_seq = 1;
    g_executed_lines = 0;
    g_packet_count = 0;
    g_nack_count = 0;
    g_last_ack_type = 0;
    g_last_ack_ack_seq = 0;
    g_last_ack_status = 0;
    g_last_ack_offset = 0;
    g_last_ack_credit = 0;
    g_route_switch_pending = false;
#if SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE
    g_panel_status_seq = 1;
#endif
    start_panel_status_task_once();
}
