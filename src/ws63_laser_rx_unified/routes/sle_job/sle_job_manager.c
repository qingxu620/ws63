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
#define SLE_JOB_EXEC_STREAM_QUEUE_REFILL_WATERMARK 4U
#define SLE_JOB_EXEC_STREAM_QUEUE_HIGH_WATERMARK 12U
#define SLE_JOB_EXEC_STREAM_THROTTLE_SLEEP_MS 5U
#define SLE_JOB_EXEC_STREAM_THROTTLE_LOG_MS 500U
#define SLE_JOB_AUTO_EXEC_TASK_STACK_SIZE 0x1000U
#define SLE_JOB_AUTO_EXEC_TASK_PRIO SLE_JOB_TASK_PRIO_JOB_EXECUTOR
#define SLE_JOB_ROUTE_SWITCH_DELAY_MS 200U
#define SLE_JOB_ROUTE_SWITCH_TASK_STACK_SIZE 0x1000U
#define SLE_JOB_PANEL_STATUS_PERIOD_MS 500U
#define SLE_JOB_PANEL_STATUS_TASK_STACK_SIZE 0x1000U
#define SLE_JOB_PANEL_STATUS_TASK_PRIO 6
#define SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE 0
#define SLE_JOB_SEND_SLOW_MS 50U
#define SLE_JOB_EXEC_START_ACK_GRACE_MS 200U
#define SLE_JOB_FAST_ACK_LOG_EVERY 32U

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
static uint32_t g_total_lines = 0;
static uint32_t g_packet_count = 0;
static uint32_t g_nack_count = 0;
static uint32_t g_diag_rx_data_count = 0;
static uint32_t g_last_data_rx_ms = 0;
static uint32_t g_exec_stream_cache_target_watermark = 0;
static osal_semaphore g_job_exec_sem;
static bool g_job_exec_sem_ready = false;
static volatile bool g_job_exec_task_started = false;
static volatile bool g_job_exec_launch_pending = false;
static osal_semaphore g_auto_exec_sem;
static bool g_auto_exec_sem_ready = false;
static volatile bool g_auto_exec_task_started = false;
static volatile bool g_auto_exec_armed = false;
static volatile bool g_auto_exec_queued = false;
static volatile uint32_t g_auto_exec_threshold = 0;
static volatile uint32_t g_auto_exec_generation = 1;
static volatile uint32_t g_auto_exec_pending_generation = 0;
static volatile uint32_t g_auto_exec_pending_job_id = 0;
static uint32_t g_data_cum_ack_offset = 0;
static uint32_t g_data_cum_ack_ms = 0;
static uint32_t g_data_cum_ack_count = 0;
static bool g_completed_job_valid = false;
static uint32_t g_completed_job_id = 0;
static uint32_t g_completed_job_total = 0;
static uint32_t g_completed_job_received = 0;
static uint16_t g_completed_job_crc = 0;
static uint32_t g_completed_job_lines = 0;
static bool g_exec_diag_reported = false;

static uint8_t  g_last_ack_type = 0;
static uint16_t g_last_ack_ack_seq = 0;
static uint8_t  g_last_ack_status = 0;
static uint32_t g_last_ack_offset = 0;
static uint32_t g_last_ack_credit = 0;
static volatile bool g_panel_status_task_started = false;
#if SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE
static uint16_t g_panel_status_seq = 1;
#endif

static void reset_auto_exec_policy(void)
{
    g_auto_exec_generation++;
    if (g_auto_exec_generation == 0U) {
        g_auto_exec_generation = 1U;
    }
    g_auto_exec_armed = false;
    g_auto_exec_queued = false;
    g_auto_exec_threshold = 0;
    g_auto_exec_pending_generation = 0;
    g_auto_exec_pending_job_id = 0;
    g_job_exec_launch_pending = false;
}

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

static void clear_completed_job_summary(void)
{
    g_completed_job_valid = false;
    g_completed_job_id = 0;
    g_completed_job_total = 0;
    g_completed_job_received = 0;
    g_completed_job_crc = 0;
    g_completed_job_lines = 0;
}

static void remember_completed_job_summary(void)
{
    uint32_t job_id = sle_job_cache_job_id();
    uint32_t total = sle_job_cache_total_size();
    uint32_t received = sle_job_cache_received();
    if (job_id == 0U || total == 0U || received < total || !sle_job_cache_is_all_received()) {
        clear_completed_job_summary();
        return;
    }

    g_completed_job_valid = true;
    g_completed_job_id = job_id;
    g_completed_job_total = total;
    g_completed_job_received = received;
    g_completed_job_crc = sle_job_cache_crc();
    g_completed_job_lines = g_executed_lines;
}

static void focus_force_off(void)
{
    g_focus_active = false;
    laser_force_off();
}

static void report_exec_diagnostics(const char *outcome)
{
    if (g_exec_diag_reported) {
        return;
    }

    sle_job_motion_diag_t motion = {0};
    sle_job_route_diag_t route = {0};
    sle_job_motion_executor_get_diag(&motion);
    sle_job_route_server_get_diag(&route);
    g_exec_diag_reported = true;

    osal_printk("[RX_EXEC_DIAG_MOTION] outcome=%s job=%u lines=%u dac=%lu skip=%lu "
                "dac_total_us=%llu dac_max_us=%u dac_gap_min_us=%u dac_gap_max_us=%u "
                "wait=%lu wait_total_us=%llu wait_max_us=%u "
                "planned_us=%llu actual_us=%llu clamp=%lu clamp_added_us=%llu "
                "step_min_us=%u step_max_us=%u late=%lu late_total_us=%llu missed=%lu max_late_us=%lu "
                "late_hist=%u,%u,%u,%u,%u,%u catchup=%lu "
                "queue_min=%u queue_avg=%u queue_max=%u queue_empty=%lu "
                "relief=%lu relief_every=%u relief_ms=%u timer_wait=%lu timer_fail=%lu "
                "timer_wait_max_us=%u timer_wake_late_max_us=%u\r\n",
                (outcome != NULL) ? outcome : "unknown",
                (unsigned int)sle_job_cache_job_id(), (unsigned int)g_executed_lines,
                motion.dac_write_count, motion.dac_skip_count,
                (unsigned long long)motion.dac_total_us, (unsigned int)motion.dac_max_us,
                (unsigned int)motion.min_dac_gap_us, (unsigned int)motion.max_dac_gap_us,
                motion.wait_call_count, (unsigned long long)motion.wait_total_us,
                (unsigned int)motion.wait_max_us,
                (unsigned long long)motion.planned_motion_us,
                (unsigned long long)motion.actual_motion_us,
                motion.short_clamped_count,
                (unsigned long long)motion.short_clamped_added_us,
                (unsigned int)motion.min_planned_step_us,
                (unsigned int)motion.max_planned_step_us,
                motion.late_sample_count,
                (unsigned long long)motion.deadline_late_total_us,
                motion.missed_sample_count, motion.max_sample_late_us,
                (unsigned int)motion.late_histogram[0],
                (unsigned int)motion.late_histogram[1],
                (unsigned int)motion.late_histogram[2],
                (unsigned int)motion.late_histogram[3],
                (unsigned int)motion.late_histogram[4],
                (unsigned int)motion.late_histogram[5],
                motion.deadline_catchup_count,
                (unsigned int)motion.queue_min_depth,
                (unsigned int)motion.queue_avg_depth,
                (unsigned int)motion.queue_max_depth,
                motion.queue_empty_count,
                motion.sched_relief_count,
                (unsigned int)SLE_JOB_MOTION_SCHED_RELIEF_INTERVAL,
                (unsigned int)SLE_JOB_MOTION_SCHED_RELIEF_MS,
                motion.timer_wait_count, motion.timer_fail_count,
                (unsigned int)motion.timer_wait_max_us,
                (unsigned int)motion.timer_wake_late_max_us);
    osal_printk("[RX_EXEC_DIAG_TIMER] outcome=%s job=%u timer_start_total_us=%llu "
                "timer_block_total_us=%llu timer_wake_late_total_us=%llu "
                "timer_wake_late_hist=%u,%u,%u,%u,%u,%u "
                "deadline_reset_count=%lu reset_discarded_us=%llu\r\n",
                (outcome != NULL) ? outcome : "unknown",
                (unsigned int)sle_job_cache_job_id(),
                (unsigned long long)motion.timer_start_total_us,
                (unsigned long long)motion.timer_block_total_us,
                (unsigned long long)motion.timer_wake_late_total_us,
                (unsigned int)motion.timer_wake_late_histogram[0],
                (unsigned int)motion.timer_wake_late_histogram[1],
                (unsigned int)motion.timer_wake_late_histogram[2],
                (unsigned int)motion.timer_wake_late_histogram[3],
                (unsigned int)motion.timer_wake_late_histogram[4],
                (unsigned int)motion.timer_wake_late_histogram[5],
                motion.deadline_reset_count,
                (unsigned long long)motion.deadline_reset_discarded_us);
    osal_printk("[RX_EXEC_DIAG_SLE] outcome=%s job=%u cb=%u cb_slow=%u cb_gap_max_ms=%u "
                "cb_max_ms=%u work=%u work_slow=%u cb_to_work_max_ms=%u "
                "work_wait_max_ms=%u work_proc_max_ms=%u q_max=%u drops=%u "
                "notify=%u notify_fail=%u notify_slow=%u notify_max_ms=%u\r\n",
                (outcome != NULL) ? outcome : "unknown",
                (unsigned int)sle_job_cache_job_id(),
                (unsigned int)route.callback_count,
                (unsigned int)route.callback_slow_count,
                (unsigned int)route.max_callback_gap_ms,
                (unsigned int)route.max_callback_ms,
                (unsigned int)route.work_count,
                (unsigned int)route.work_slow_count,
                (unsigned int)route.max_callback_to_work_ms,
                (unsigned int)route.max_work_wait_ms,
                (unsigned int)route.max_work_process_ms,
                (unsigned int)route.work_max_used,
                (unsigned int)route.work_dropped,
                (unsigned int)route.notify_count,
                (unsigned int)route.notify_fail_count,
                (unsigned int)route.notify_slow_count,
                (unsigned int)route.max_notify_ms);
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
    uint32_t t_encode = (uint32_t)uapi_systick_get_ms();
    if (!sle_job_packet_encode(type, 0, next_resp_seq(), payload, payload_len,
                           buf, sizeof(buf), &out_len)) {
        osal_printk("[JOB_RX] encode response fail type=0x%02x\r\n", type);
        return ERRCODE_SLE_FAIL;
    }
    uint32_t encode_ms = (uint32_t)uapi_systick_get_ms() - t_encode;
    uint32_t t_send = (uint32_t)uapi_systick_get_ms();
    errcode_t ret = sle_job_route_server_send_packet(buf, out_len);
    uint32_t notify_ms = (uint32_t)uapi_systick_get_ms() - t_send;
    uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_encode;
    if (ret != ERRCODE_SLE_SUCCESS || total_ms >= SLE_JOB_SEND_SLOW_MS) {
        osal_printk("[RX_SEND_TIMING] type=0x%02x len=%u ret=0x%x encode_ms=%u notify_ms=%u "
                    "total_ms=%u owner=%u conns=%u state=%s rx=%u consumed=%u avail=%u q=%u motion_busy=%u lines=%u\r\n",
                    type, (unsigned int)out_len, (unsigned int)ret,
                    (unsigned int)encode_ms, (unsigned int)notify_ms,
                    (unsigned int)total_ms,
                    (unsigned int)sle_job_route_server_get_owner_conn_id(),
                    (unsigned int)sle_job_route_server_get_connection_count(),
                    state_name(g_state),
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available(),
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                    (unsigned int)g_executed_lines);
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
    st.completed_lines = sle_job_motion_executor_completed_line();
    st.total_lines = g_total_lines;
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

static errcode_t send_ack_with_reason(uint8_t ack_type, uint16_t ack_seq,
                                      sle_job_status_t status, const char *reason)
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
    if (ack_type == SLE_JOB_PKT_JOB_DATA && status == SLE_JOB_STATUS_OK &&
        send_ret == ERRCODE_SLE_SUCCESS) {
        g_data_cum_ack_offset = ack.offset;
        g_data_cum_ack_ms = (uint32_t)uapi_systick_get_ms();
    }
    if (ack_type != SLE_JOB_PKT_JOB_DATA ||
        (SLE_JOB_DIAG_LOG && (g_diag_rx_data_count <= 8U || status != SLE_JOB_STATUS_OK ||
                              send_ret != ERRCODE_SLE_SUCCESS)) ||
        send_ret != ERRCODE_SLE_SUCCESS ||
        ack_send_ms >= SLE_JOB_SEND_SLOW_MS) {
        uint32_t now = (uint32_t)uapi_systick_get_ms();
        uint32_t since_data_ms = (g_last_data_rx_ms == 0U) ? 0U : (uint32_t)(now - g_last_data_rx_ms);
        osal_printk("[RX_ACK] t=%u type=0x%02x seq=%u st=%u off=%u credit=%u ret=0x%x "
                    "send_ms=%u state=%s rx=%u consumed=%u avail=%u q=%u motion_busy=%u "
                    "lines=%u since_data_ms=%u reason=%s\r\n",
                    (unsigned int)uapi_systick_get_ms(), (unsigned int)ack_type,
                    (unsigned int)ack_seq, (unsigned int)status,
                    (unsigned int)ack.offset, (unsigned int)ack.credit,
                    (unsigned int)send_ret, (unsigned int)ack_send_ms,
                    state_name(g_state),
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available(),
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                    (unsigned int)g_executed_lines,
                    (unsigned int)since_data_ms,
                    (reason != NULL) ? reason : "none");
    }
    if (status != SLE_JOB_STATUS_OK) {
        osal_printk("[JOB_RX_NACK_SEND] type=%u seq=%u status=%u state=%s reason=%s\r\n",
                    (unsigned int)ack_type, (unsigned int)ack_seq, (unsigned int)status,
                    state_name(g_state), (reason != NULL) ? reason : "unspecified");
        g_nack_count++;
    }
    return send_ret;
}

static void send_ack(uint8_t ack_type, uint16_t ack_seq, sle_job_status_t status)
{
    (void)send_ack_with_reason(ack_type, ack_seq, status, NULL);
}

static bool data_fast_cum_ack_active(const sle_job_packet_view_t *pkt)
{
#if SLE_JOB_DATA_FAST_CUM_ACK_ENABLE
    if (pkt == NULL || pkt->type != SLE_JOB_PKT_JOB_DATA ||
        (pkt->flags & SLE_JOB_PACKET_FLAG_DATA_FAST_ACK) == 0U) {
        return false;
    }
    return g_state == SLE_JOB_STATE_RECEIVING_JOB ||
           g_state == SLE_JOB_STATE_EXECUTING ||
           g_state == SLE_JOB_STATE_PAUSED;
#else
    unused(pkt);
    return false;
#endif
}

static bool data_fast_cum_ack_due(uint32_t rx_offset, uint32_t total_size)
{
    uint32_t delta = (rx_offset >= g_data_cum_ack_offset) ? (rx_offset - g_data_cum_ack_offset) : 0U;
    uint32_t now = (uint32_t)uapi_systick_get_ms();
    uint32_t age_ms = (g_data_cum_ack_ms == 0U) ? 0U : (uint32_t)(now - g_data_cum_ack_ms);

    if (g_data_cum_ack_ms == 0U) {
        return true;
    }
    if (total_size > 0U && rx_offset >= total_size) {
        return true;
    }
#if SLE_JOB_DATA_FAST_CUM_ACK_INTERVAL_MS > 0
    return delta >= SLE_JOB_DATA_FAST_CUM_ACK_BYTES ||
           age_ms >= SLE_JOB_DATA_FAST_CUM_ACK_INTERVAL_MS;
#else
    unused(age_ms);
    return delta >= SLE_JOB_DATA_FAST_CUM_ACK_BYTES;
#endif
}

static bool maybe_send_data_progress_ack(uint16_t seq, bool fast_active, bool force_ack,
                                         sle_job_status_t status, const char *fail_reason,
                                         uint32_t *ack_ms_out, errcode_t *ack_ret_out)
{
    if (ack_ms_out != NULL) {
        *ack_ms_out = 0;
    }
    if (ack_ret_out != NULL) {
        *ack_ret_out = ERRCODE_SLE_FAIL;
    }

    uint32_t rx_offset = sle_job_cache_received();
    uint32_t total_size = sle_job_cache_total_size();
    if (!fast_active || status != SLE_JOB_STATUS_OK || force_ack ||
        data_fast_cum_ack_due(rx_offset, total_size)) {
        uint32_t old_ack_offset = g_data_cum_ack_offset;
        uint32_t old_ack_ms = g_data_cum_ack_ms;
        uint32_t now = (uint32_t)uapi_systick_get_ms();
        uint32_t age_ms = (old_ack_ms == 0U) ? 0U : (uint32_t)(now - old_ack_ms);
        uint32_t t_ack = now;
        errcode_t ack_ret = send_ack_with_reason(
            SLE_JOB_PKT_JOB_DATA, seq, status,
            (status == SLE_JOB_STATUS_OK) ?
            (fast_active ? (force_ack ? "force_cum" : "fast_cum") : NULL) : fail_reason);
        uint32_t ack_ms = (uint32_t)uapi_systick_get_ms() - t_ack;
        if (ack_ms_out != NULL) {
            *ack_ms_out = ack_ms;
        }
        if (ack_ret_out != NULL) {
            *ack_ret_out = ack_ret;
        }
        if (fast_active) {
            g_data_cum_ack_count++;
            uint32_t delta = (rx_offset >= old_ack_offset) ? (rx_offset - old_ack_offset) : 0U;
            bool final_ack = total_size > 0U && rx_offset >= total_size;
            bool log_due = final_ack || ack_ms >= SLE_JOB_SEND_SLOW_MS ||
                            (SLE_JOB_FAST_ACK_LOG_EVERY > 0U &&
                             (g_data_cum_ack_count % SLE_JOB_FAST_ACK_LOG_EVERY) == 0U);
            if (log_due) {
                osal_printk("[RX_DATA_FAST_ACK] count=%u t=%u seq=%u st=%u off=%u old_off=%u "
                            "delta=%u age_ms=%u ack_ms=%u force=%u free=%u state=%s\r\n",
                            (unsigned int)g_data_cum_ack_count,
                            (unsigned int)uapi_systick_get_ms(),
                            (unsigned int)seq,
                            (unsigned int)status,
                            (unsigned int)rx_offset,
                            (unsigned int)old_ack_offset,
                            (unsigned int)delta,
                            (unsigned int)age_ms,
                            (unsigned int)ack_ms,
                            (unsigned int)(force_ack ? 1U : 0U),
                            (unsigned int)sle_job_cache_free(),
                            state_name(g_state));
            }
        }
        return true;
    }

    return false;
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
    resp.completed_lines = sle_job_motion_executor_completed_line();
    resp.total_lines = g_total_lines;
    if (resp.job_id == 0U && g_completed_job_valid) {
        resp.job_id = g_completed_job_id;
        resp.received_size = g_completed_job_received;
        resp.total_size = g_completed_job_total;
        resp.executed_lines = g_completed_job_lines;
        resp.completed_lines = g_completed_job_lines;
    }
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
        uint32_t t_resend = (uint32_t)uapi_systick_get_ms();
        osal_printk("[JOB_RX_SEQ] seq=%u expected=%u duplicate=1 cached_type=%u cached_status=%u cached_off=%u cached_credit=%u\r\n",
                    (unsigned int)pkt->seq, (unsigned int)g_expected_seq,
                    (unsigned int)g_last_ack_type, (unsigned int)g_last_ack_status,
                    (unsigned int)g_last_ack_offset, (unsigned int)g_last_ack_credit);
        send_ack(g_last_ack_type, g_last_ack_ack_seq, g_last_ack_status);
        uint32_t resend_ms = (uint32_t)uapi_systick_get_ms() - t_resend;
        osal_printk("[JOB_RX_DUP_ACK] seq=%u cached_seq=%u resend_ms=%u state=%s "
                    "rx=%u consumed=%u avail=%u q=%u motion_busy=%u lines=%u\r\n",
                    (unsigned int)pkt->seq, (unsigned int)g_last_ack_ack_seq,
                    (unsigned int)resend_ms, state_name(g_state),
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available(),
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                    (unsigned int)g_executed_lines);
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

static bool wait_motion_idle(uint32_t inactive_timeout_ms)
{
    uint32_t wait_start = (uint32_t)uapi_systick_get_ms();
    uint32_t inactive_start = wait_start;
    uint32_t last_log_ms = inactive_start;
    unsigned long last_activity = sle_job_motion_executor_last_activity_ms();

    while (sle_job_motion_executor_is_busy()) {
        if (sle_job_motion_executor_abort_requested()) {
            osal_printk("[JOB_MOTION_DRAIN_ABORT] q=%u enq=%lu exec=%lu\r\n",
                        (unsigned int)sle_job_motion_executor_queue_depth(),
                        sle_job_motion_executor_enqueued_count(),
                        sle_job_motion_executor_executed_count());
            return false;
        }
        uint32_t now = (uint32_t)uapi_systick_get_ms();
        unsigned long activity = sle_job_motion_executor_last_activity_ms();
        if (activity != 0UL && activity != last_activity) {
            last_activity = activity;
            inactive_start = now;
        }
        if ((uint32_t)(now - inactive_start) >= inactive_timeout_ms) {
            osal_printk("[JOB_MOTION_DRAIN_TIMEOUT] inactive_ms=%u q=%u busy=%u "
                        "enq=%lu exec=%lu last_activity=%lu\r\n",
                        (unsigned int)(now - inactive_start),
                        (unsigned int)sle_job_motion_executor_queue_depth(),
                        (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                        sle_job_motion_executor_enqueued_count(),
                        sle_job_motion_executor_executed_count(),
                        last_activity);
            return false;
        }
        if ((uint32_t)(now - last_log_ms) >= 5000U) {
            osal_printk("[JOB_MOTION_DRAIN] wait_ms=%u inactive_ms=%u q=%u busy=%u "
                        "enq=%lu exec=%lu\r\n",
                        (unsigned int)(now - wait_start),
                        (unsigned int)(now - inactive_start),
                        (unsigned int)sle_job_motion_executor_queue_depth(),
                        (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                        sle_job_motion_executor_enqueued_count(),
                        sle_job_motion_executor_executed_count());
            last_log_ms = now;
        }
        osal_msleep(1);
    }
    return true;
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
    report_exec_diagnostics((reason != NULL) ? reason : "safe_stop");
    clear_completed_job_summary();
    focus_force_off();
    g_abort_requested = true;
    g_pause_requested = false;
    sle_job_motion_executor_request_abort();
    sle_job_motion_executor_flush();
    sle_job_motion_cmd_t cmd;
    sle_job_gcode_processor_build_emergency_stop(&cmd);
    sle_job_motion_executor_execute(&cmd);
    laser_force_off();
    reset_auto_exec_policy();
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
    uint32_t source_line = g_executed_lines + 1U;
    if (cmd_count == 0) {
        memset(&cmds[0], 0, sizeof(cmds[0]));
        cmds[0].cmd = SLE_JOB_CMD_PROGRESS_MARK;
        cmd_count = 1;
    }
    cmds[cmd_count - 1].completion_line = source_line;
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
        if (!wait_motion_idle(SLE_JOB_MOTION_END_DRAIN_TIMEOUT_MS)) {
            return false;
        }
        laser_force_off();
    }
    return true;
}

static void throttle_streaming_executor(void)
{
    if (sle_job_cache_is_all_received()) {
        return;
    }

    uint32_t throttle_start_ms = 0;
    uint32_t last_log_ms = 0;

    while (!g_abort_requested && !g_pause_requested && !sle_job_cache_is_all_received()) {
        uint32_t avail = sle_job_cache_available();
        uint16_t queue = sle_job_motion_executor_queue_depth();

        uint32_t target = g_exec_stream_cache_target_watermark;
        if (target == 0U) {
            break;
        }

        if (avail >= target &&
            queue < SLE_JOB_EXEC_STREAM_QUEUE_HIGH_WATERMARK) {
            break;
        }

        if (queue < SLE_JOB_EXEC_STREAM_QUEUE_REFILL_WATERMARK && avail > 0U) {
            break;
        }

        uint32_t now = (uint32_t)uapi_systick_get_ms();
        if (throttle_start_ms == 0U) {
            throttle_start_ms = now;
            last_log_ms = now;
        }
        if ((uint32_t)(now - last_log_ms) >= SLE_JOB_EXEC_STREAM_THROTTLE_LOG_MS) {
            osal_printk("[JOB_EXEC_THROTTLE] wait_ms=%u rx=%u consumed=%u avail=%u "
                        "target=%u q=%u refill=%u high=%u motion_busy=%u lines=%u\r\n",
                        (unsigned int)(now - throttle_start_ms),
                        (unsigned int)sle_job_cache_received(),
                        (unsigned int)sle_job_cache_consumed(),
                        (unsigned int)avail,
                        (unsigned int)target,
                        (unsigned int)queue,
                        (unsigned int)SLE_JOB_EXEC_STREAM_QUEUE_REFILL_WATERMARK,
                        (unsigned int)SLE_JOB_EXEC_STREAM_QUEUE_HIGH_WATERMARK,
                        (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                        (unsigned int)g_executed_lines);
            last_log_ms = now;
        }
        osal_msleep(SLE_JOB_EXEC_STREAM_THROTTLE_SLEEP_MS);
    }
}

static int job_exec_task(void *arg)
{
    unused(arg);
    char line[SLE_JOB_LINE_MAX];

    if (g_abort_requested || g_pause_requested) {
        osal_printk("[JOB_EXEC] %s already requested, exiting\r\n",
                    g_pause_requested ? "pause" : "abort");
        g_exec_active = false;
        return 0;
    }

    uint32_t wait_start_ms = 0;

    while (!g_abort_requested && !g_pause_requested) {
        int line_len = sle_job_cache_read_line((uint8_t *)line, sizeof(line));
        if (line_len == -2) {
            sle_job_manager_safe_stop("line-too-long");
            g_exec_active = false;
            return 0;
        }
        if (line_len < 0) {
            sle_job_manager_safe_stop("cache-read-line-fail");
            g_exec_active = false;
            return 0;
        }
        if (line_len == 0) {
            if (sle_job_cache_is_all_received()) {
                break;
            }
            uint16_t queue = sle_job_motion_executor_queue_depth();
            bool motion_busy = sle_job_motion_executor_is_busy();
            if (motion_busy || queue > 0U) {
                wait_start_ms = 0;
                osal_msleep(1);
                continue;
            }
            if (wait_start_ms == 0) {
                wait_start_ms = (uint32_t)uapi_systick_get_ms();
            }
            uint32_t wait_elapsed = (uint32_t)uapi_systick_get_ms() - wait_start_ms;
            if (wait_elapsed >= SLE_JOB_EXEC_WAIT_DATA_TIMEOUT_MS) {
                osal_printk("[RX_WAIT_TIMEOUT] consumed=%u available=%u total=%u all_received=%d "
                            "state=%s q=%u motion_busy=%u\r\n",
                            (unsigned int)sle_job_cache_received(),
                            (unsigned int)sle_job_cache_available(),
                            (unsigned int)sle_job_cache_total_size(),
                            (int)sle_job_cache_is_all_received(),
                            state_name(g_state),
                            (unsigned int)queue,
                            (unsigned int)(motion_busy ? 1U : 0U));
                sle_job_manager_safe_stop("exec-wait-timeout");
                g_exec_active = false;
                return 0;
            }
            osal_msleep(1);
            continue;
        }
        wait_start_ms = 0;

        if (line[line_len - 1] == '\r' || line[line_len - 1] == '\n') {
            line[line_len - 1] = '\0';
        } else {
            line[line_len] = '\0';
        }
        strip_line(line);
        if (line[0] != '\0') {
            if (!execute_line(line)) {
                if (g_pause_requested) {
                    laser_force_off();
                    g_exec_active = false;
                    report_exec_diagnostics("paused");
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
    }

    if (g_pause_requested) {
        laser_force_off();
        g_exec_active = false;
        report_exec_diagnostics("paused");
        osal_printk("[JOB_EXEC] paused job=%u lines=%u consumed=%u received=%u\r\n",
                    (unsigned int)sle_job_cache_job_id(),
                    (unsigned int)g_executed_lines,
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_received());
        return 0;
    }

    if (!wait_motion_idle(SLE_JOB_MOTION_END_DRAIN_TIMEOUT_MS)) {
        sle_job_manager_safe_stop("motion-drain-timeout");
        g_exec_active = false;
        return 0;
    }
    laser_force_off();
    focus_force_off();
    if (!g_abort_requested) {
        g_state = SLE_JOB_STATE_IDLE;
        remember_completed_job_summary();
        int32_t x_um = (int32_t)(sle_job_motion_executor_get_x() * 1000.0);
        int32_t y_um = (int32_t)(sle_job_motion_executor_get_y() * 1000.0);
        report_exec_diagnostics("done");
        osal_printk("[JOB_EXEC] done job=%u lines=%u x_um=%d y_um=%d "
                    "seg=%lu short=%lu late=%lu missed=%lu max_late_us=%lu "
                    "q=%u min_mark_us=%u sample_us=%u profile=%u "
                    "dac_write=%lu dac_skip=%lu sleep_th_us=%u\r\n",
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
                    (unsigned int)SLE_JOB_MOTION_SPEED_PROFILE,
                    sle_job_motion_executor_dac_write_count(),
                    sle_job_motion_executor_dac_skip_count(),
                    (unsigned int)SLE_JOB_MOTION_SLEEP_THRESHOLD_US);
        sle_job_cache_clear();
        reset_auto_exec_policy();
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

static int job_exec_worker_task(void *arg)
{
    unused(arg);
    while (1) {
        if (osal_sem_down(&g_job_exec_sem) != OSAL_SUCCESS) {
            osal_msleep(1);
            continue;
        }
        if (!g_job_exec_launch_pending) {
            continue;
        }

        g_job_exec_launch_pending = false;
        (void)job_exec_task(NULL);
    }
    return 0;
}

static void start_job_exec_task_once(void)
{
    if (g_job_exec_task_started) {
        return;
    }
    if (!g_job_exec_sem_ready &&
        osal_sem_init(&g_job_exec_sem, 0) != OSAL_SUCCESS) {
        osal_printk("[JOB_EXEC_WORKER] semaphore init failed\r\n");
        return;
    }
    g_job_exec_sem_ready = true;

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(job_exec_worker_task, NULL, "job_exec",
                                          SLE_JOB_EXEC_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[JOB_EXEC_WORKER] create task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_TASK_PRIO_JOB_EXECUTOR) != OSAL_SUCCESS) {
        osal_printk("[JOB_EXEC_WORKER] set priority failed\r\n");
    }
    g_job_exec_task_started = true;
    osal_kfree(task);
    osal_kthread_unlock();
    osal_printk("[JOB_EXEC_WORKER] ready prio=%u\r\n",
                (unsigned int)SLE_JOB_TASK_PRIO_JOB_EXECUTOR);
}

static sle_job_status_t launch_job_execution(void)
{
    if (!g_job_exec_sem_ready || !g_job_exec_task_started ||
        g_job_exec_launch_pending) {
        return SLE_JOB_STATUS_INTERNAL_ERROR;
    }

    sle_job_motion_executor_clear_abort();
    g_job_exec_launch_pending = true;
    osal_sem_up(&g_job_exec_sem);
    return SLE_JOB_STATUS_OK;
}

static int auto_exec_task(void *arg)
{
    unused(arg);
    while (1) {
        if (osal_sem_down(&g_auto_exec_sem) != OSAL_SUCCESS) {
            osal_msleep(1);
            continue;
        }

        uint32_t generation = g_auto_exec_pending_generation;
        uint32_t job_id = g_auto_exec_pending_job_id;
        uint32_t threshold = g_auto_exec_threshold;
        if (!g_auto_exec_queued || generation != g_auto_exec_generation ||
            job_id == 0U || job_id != sle_job_cache_job_id() ||
            g_abort_requested || g_pause_requested || !g_exec_active ||
            g_state != SLE_JOB_STATE_EXECUTING) {
            continue;
        }

        g_auto_exec_queued = false;
        sle_job_status_t st = launch_job_execution();
        if (st != SLE_JOB_STATUS_OK) {
            osal_printk("[AUTO_EXEC] launch failed job=%u generation=%u st=%u\r\n",
                        (unsigned int)job_id, (unsigned int)generation,
                        (unsigned int)st);
            sle_job_manager_safe_stop("auto-exec-launch-fail");
            continue;
        }
        osal_printk("[AUTO_EXEC] launched job=%u generation=%u threshold=%u "
                    "rx=%u consumed=%u avail=%u\r\n",
                    (unsigned int)job_id, (unsigned int)generation,
                    (unsigned int)threshold,
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available());
    }
    return 0;
}

static void start_auto_exec_task_once(void)
{
    if (g_auto_exec_task_started) {
        return;
    }
    if (!g_auto_exec_sem_ready &&
        osal_sem_init(&g_auto_exec_sem, 0) != OSAL_SUCCESS) {
        osal_printk("[AUTO_EXEC] semaphore init failed\r\n");
        return;
    }
    g_auto_exec_sem_ready = true;

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(auto_exec_task, NULL, "auto_exec",
                                          SLE_JOB_AUTO_EXEC_TASK_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[AUTO_EXEC] create task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_AUTO_EXEC_TASK_PRIO) != OSAL_SUCCESS) {
        osal_printk("[AUTO_EXEC] set priority failed\r\n");
    }
    g_auto_exec_task_started = true;
    osal_kfree(task);
    osal_kthread_unlock();
}

static bool auto_exec_ready_to_schedule(void)
{
    return g_auto_exec_armed && !g_auto_exec_queued &&
           (g_state == SLE_JOB_STATE_RECEIVING_JOB ||
            g_state == SLE_JOB_STATE_JOB_READY) &&
           !g_exec_active &&
           sle_job_cache_received() >= g_auto_exec_threshold;
}

static void maybe_schedule_auto_execution(void)
{
    if (!auto_exec_ready_to_schedule()) {
        return;
    }

    g_auto_exec_armed = false;
    g_auto_exec_queued = true;
    g_auto_exec_pending_generation = g_auto_exec_generation;
    g_auto_exec_pending_job_id = sle_job_cache_job_id();
    g_exec_active = true;
    g_abort_requested = false;
    g_pause_requested = false;
    g_exec_stream_cache_target_watermark = sle_job_cache_available();
    g_state = SLE_JOB_STATE_EXECUTING;

    osal_printk("[RX_EXEC_DIAG_BEGIN] job=%u mode=auto preroll=%u sample_us=%u "
                "delay_chunk_us=%u sleep_th_us=%u profile=%u relief_every=%u relief_ms=%u "
                "timer_th_us=%u timer_tail_us=%u\r\n",
                (unsigned int)g_auto_exec_pending_job_id,
                (unsigned int)g_auto_exec_threshold,
                (unsigned int)SLE_JOB_MOTION_SAMPLE_PERIOD_US,
                (unsigned int)SLE_JOB_MOTION_DELAY_CHUNK_US,
                (unsigned int)SLE_JOB_MOTION_SLEEP_THRESHOLD_US,
                (unsigned int)SLE_JOB_MOTION_SPEED_PROFILE,
                (unsigned int)SLE_JOB_MOTION_SCHED_RELIEF_INTERVAL,
                (unsigned int)SLE_JOB_MOTION_SCHED_RELIEF_MS,
                (unsigned int)SLE_JOB_MOTION_TIMER_THRESHOLD_US,
                (unsigned int)SLE_JOB_MOTION_TIMER_TAIL_US);
    osal_printk("[AUTO_EXEC] threshold reached job=%u generation=%u threshold=%u "
                "rx=%u consumed=%u avail=%u q=%u\r\n",
                (unsigned int)g_auto_exec_pending_job_id,
                (unsigned int)g_auto_exec_pending_generation,
                (unsigned int)g_auto_exec_threshold,
                (unsigned int)sle_job_cache_received(),
                (unsigned int)sle_job_cache_consumed(),
                (unsigned int)sle_job_cache_available(),
                (unsigned int)sle_job_motion_executor_queue_depth());
    if (!g_auto_exec_sem_ready) {
        sle_job_manager_safe_stop("auto-exec-signal-fail");
        return;
    }
    osal_sem_up(&g_auto_exec_sem);
}

static void handle_job_begin(const sle_job_packet_view_t *pkt)
{
    bool auto_exec = false;
    uint32_t job_id = 0;
    uint32_t total_size = 0;
    uint16_t job_crc16 = 0;
    uint32_t exec_preroll_bytes = 0;
    uint32_t total_lines = 0;

    if (pkt->len == sizeof(sle_job_begin_payload_t)) {
        sle_job_begin_payload_t begin;
        memcpy(&begin, pkt->payload, sizeof(begin));
        job_id = begin.job_id;
        total_size = begin.total_size;
        job_crc16 = begin.job_crc16;
    } else if (pkt->len == sizeof(sle_job_begin_stream_payload_t)) {
        sle_job_begin_stream_payload_t begin;
        memcpy(&begin, pkt->payload, sizeof(begin));
        if (begin.options != SLE_JOB_BEGIN_OPTION_AUTO_EXEC_PREROLL ||
            begin.exec_preroll_bytes == 0U ||
            begin.exec_preroll_bytes >= begin.total_size ||
            begin.exec_preroll_bytes > SLE_JOB_EXEC_PREROLL_MAX_BYTES ||
            !g_auto_exec_task_started || !g_job_exec_task_started) {
            osal_printk("[JOB_BEGIN] reject auto options=0x%x preroll=%u total=%u "
                        "max=%u auto_worker=%u exec_worker=%u\r\n",
                        (unsigned int)begin.options,
                        (unsigned int)begin.exec_preroll_bytes,
                        (unsigned int)begin.total_size,
                        (unsigned int)SLE_JOB_EXEC_PREROLL_MAX_BYTES,
                        g_auto_exec_task_started ? 1U : 0U,
                        g_job_exec_task_started ? 1U : 0U);
            send_ack(pkt->type, pkt->seq,
                     (g_auto_exec_task_started && g_job_exec_task_started) ?
                     SLE_JOB_STATUS_BAD_JOB : SLE_JOB_STATUS_INTERNAL_ERROR);
            return;
        }
        auto_exec = true;
        job_id = begin.job_id;
        total_size = begin.total_size;
        job_crc16 = begin.job_crc16;
        exec_preroll_bytes = begin.exec_preroll_bytes;
    } else if (pkt->len == sizeof(sle_job_begin_stream_v2_payload_t)) {
        sle_job_begin_stream_v2_payload_t begin;
        memcpy(&begin, pkt->payload, sizeof(begin));
        bool begin_auto_exec = begin.options == SLE_JOB_BEGIN_OPTION_AUTO_EXEC_PREROLL;
        bool valid_manual = begin.options == 0U && begin.exec_preroll_bytes == 0U;
        bool valid_auto = begin_auto_exec && begin.exec_preroll_bytes > 0U &&
                          begin.exec_preroll_bytes < begin.total_size &&
                          begin.exec_preroll_bytes <= SLE_JOB_EXEC_PREROLL_MAX_BYTES &&
                          g_auto_exec_task_started && g_job_exec_task_started;
        if ((!valid_manual && !valid_auto) || begin.total_lines == 0U) {
            send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
            return;
        }
        auto_exec = begin_auto_exec;
        job_id = begin.job_id;
        total_size = begin.total_size;
        job_crc16 = begin.job_crc16;
        exec_preroll_bytes = begin.exec_preroll_bytes;
        total_lines = begin.total_lines;
    } else {
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
        return;
    }

    if (g_exec_active || g_state == SLE_JOB_STATE_EXECUTING) {
        osal_printk("[JOB_BEGIN] reject: still executing state=%s\r\n", state_name(g_state));
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_STATE);
        return;
    }

    sle_job_status_t st = sle_job_cache_begin(job_id, total_size, job_crc16);
    if (st == SLE_JOB_STATUS_OK) {
        reset_auto_exec_policy();
        g_auto_exec_armed = auto_exec;
        g_auto_exec_threshold = exec_preroll_bytes;
        clear_completed_job_summary();
        sle_job_motion_executor_clear_abort();
        sle_job_motion_executor_reset_stats();
        sle_job_route_server_reset_diag();
        g_exec_diag_reported = false;
        g_state = SLE_JOB_STATE_RECEIVING_JOB;
        g_abort_requested = false;
        g_pause_requested = false;
        g_executed_lines = 0;
        g_total_lines = total_lines;
        g_diag_rx_data_count = 0;
        g_last_data_rx_ms = 0;
        g_exec_stream_cache_target_watermark = 0;
        g_data_cum_ack_offset = 0;
        g_data_cum_ack_ms = (uint32_t)uapi_systick_get_ms();
        g_data_cum_ack_count = 0;
        seq_commit(pkt->seq);
        osal_printk("[JOB_BEGIN] accepted job=%u total=%u lines=%u auto_exec=%u preroll=%u "
                    "generation=%u\r\n",
                     (unsigned int)job_id, (unsigned int)total_size,
                     (unsigned int)total_lines,
                    auto_exec ? 1U : 0U, (unsigned int)exec_preroll_bytes,
                    (unsigned int)g_auto_exec_generation);
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
    uint32_t prev_data_rx_ms = g_last_data_rx_ms;
    uint32_t rx_gap_ms = (prev_data_rx_ms == 0U) ? 0U : (uint32_t)(t_start - prev_data_rx_ms);
    g_last_data_rx_ms = t_start;

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

    if (sle_job_cache_is_duplicate_data(hdr.job_id, hdr.offset, hdr.data_len)) {
        seq_commit(pkt->seq);
        g_diag_rx_data_count++;
        uint32_t data_index = g_diag_rx_data_count;
        uint32_t t_ack = (uint32_t)uapi_systick_get_ms();
        send_ack_with_reason(pkt->type, pkt->seq, SLE_JOB_STATUS_OK, "dup_data");
        uint32_t ack_ms = (uint32_t)uapi_systick_get_ms() - t_ack;
        if (auto_exec_ready_to_schedule() &&
            g_data_cum_ack_offset >= sle_job_cache_received()) {
            osal_printk("[AUTO_EXEC_ACK_BARRIER] duplicate seq=%u off=%u ack_ms=%u launch=1\r\n",
                        (unsigned int)pkt->seq,
                        (unsigned int)g_data_cum_ack_offset,
                        (unsigned int)ack_ms);
            maybe_schedule_auto_execution();
        }
        uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_start;
        osal_printk("[JOB_DATA_DUP] seq=%u data_idx=%u job=%u off=%u len=%u "
                    "rx=%u consumed=%u ack_ms=%u total_ms=%u state=%s\r\n",
                    (unsigned int)pkt->seq, (unsigned int)data_index,
                    (unsigned int)hdr.job_id, (unsigned int)hdr.offset,
                    (unsigned int)hdr.data_len,
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)ack_ms, (unsigned int)total_ms,
                    state_name(g_state));
        return;
    }

    uint32_t t_write = (uint32_t)uapi_systick_get_ms();
    sle_job_status_t st = sle_job_cache_write(hdr.job_id, hdr.offset,
                                          &pkt->payload[sizeof(sle_job_data_payload_t)], hdr.data_len);
    uint32_t write_ms = (uint32_t)uapi_systick_get_ms() - t_write;
    if (st == SLE_JOB_STATUS_OK) {
        seq_commit(pkt->seq);
        uint32_t total_size = sle_job_cache_total_size();
        if (total_size > 0U && sle_job_cache_received() >= total_size &&
            !sle_job_cache_is_all_received()) {
            sle_job_status_t finish_st = sle_job_cache_finish(hdr.job_id, total_size, 0);
            if (finish_st == SLE_JOB_STATUS_OK) {
                sle_job_cache_set_all_received();
                if (g_state == SLE_JOB_STATE_RECEIVING_JOB) {
                    g_state = SLE_JOB_STATE_JOB_READY;
                }
                osal_printk("[JOB_DATA_AUTO_FINISH] t=%u seq=%u job=%u rx=%u total=%u "
                            "crc=0x%04x state=%s\r\n",
                            (unsigned int)uapi_systick_get_ms(),
                            (unsigned int)pkt->seq,
                            (unsigned int)hdr.job_id,
                            (unsigned int)sle_job_cache_received(),
                            (unsigned int)total_size,
                            (unsigned int)sle_job_cache_crc(),
                            state_name(g_state));
            } else {
                st = finish_st;
                if (g_state == SLE_JOB_STATE_RECEIVING_JOB) {
                    g_state = SLE_JOB_STATE_ERROR;
                }
                g_abort_requested = true;
                g_pause_requested = false;
                osal_printk("[JOB_DATA_AUTO_FINISH_FAIL] t=%u seq=%u job=%u rx=%u total=%u "
                            "st=%u crc=0x%04x state=%s\r\n",
                            (unsigned int)uapi_systick_get_ms(),
                            (unsigned int)pkt->seq,
                            (unsigned int)hdr.job_id,
                            (unsigned int)sle_job_cache_received(),
                            (unsigned int)total_size,
                            (unsigned int)finish_st,
                            (unsigned int)sle_job_cache_crc(),
                            state_name(g_state));
            }
        }
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
    uint32_t ack_ms = 0;
    bool fast_ack = data_fast_cum_ack_active(pkt);
    bool auto_exec_due = st == SLE_JOB_STATUS_OK && auto_exec_ready_to_schedule();
    bool force_ack = (pkt->flags & SLE_JOB_PACKET_FLAG_DATA_FORCE_ACK) != 0U ||
                     auto_exec_due;
    uint32_t pre_ack_ms = (uint32_t)uapi_systick_get_ms() - t_start;
    errcode_t ack_ret = ERRCODE_SLE_FAIL;
    bool ack_sent = maybe_send_data_progress_ack(pkt->seq, fast_ack, force_ack,
                                                  st, "cache_write", &ack_ms, &ack_ret);
    if (st == SLE_JOB_STATUS_OK && sle_job_cache_total_size() > 0U &&
        sle_job_cache_received() >= sle_job_cache_total_size()) {
        osal_printk("[RX_FINAL_ACK_SUBMIT] seq=%u off=%u ret=0x%x ack_ms=%u committed=%u\r\n",
                    (unsigned int)pkt->seq,
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)ack_ret,
                    (unsigned int)ack_ms,
                    (unsigned int)(g_data_cum_ack_offset >= sle_job_cache_received()));
    }
    if (st == SLE_JOB_STATUS_OK) {
        bool auto_exec_ack_ready = !auto_exec_due ||
                                   (ack_sent &&
                                    g_data_cum_ack_offset >= sle_job_cache_received());
        if (auto_exec_due) {
            osal_printk("[AUTO_EXEC_ACK_BARRIER] seq=%u off=%u ack_off=%u ack_ms=%u "
                        "ack_ready=%u launch=%u\r\n",
                        (unsigned int)pkt->seq,
                        (unsigned int)sle_job_cache_received(),
                        (unsigned int)g_data_cum_ack_offset,
                        (unsigned int)ack_ms,
                        auto_exec_ack_ready ? 1U : 0U,
                        auto_exec_ack_ready ? 1U : 0U);
        }
        if (auto_exec_ack_ready) {
            maybe_schedule_auto_execution();
        }
    }
    uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_start;
    if (rx_should_log_data_timing(data_index, total_ms) || st != SLE_JOB_STATUS_OK) {
        osal_printk("[RX_TIMING] seq=%u data_idx=%u off=%u len=%u gap_ms=%u write_ms=%u "
                    "pre_ack_ms=%u ack_sent=%u ack_ms=%u total_ms=%u st=%u free=%u "
                    "rx=%u consumed=%u avail=%u q=%u motion_busy=%u lines=%u state=%s\r\n",
                    (unsigned int)pkt->seq, (unsigned int)data_index,
                    (unsigned int)hdr.offset, (unsigned int)hdr.data_len,
                    (unsigned int)rx_gap_ms, (unsigned int)write_ms,
                    (unsigned int)pre_ack_ms,
                    (unsigned int)(ack_sent ? 1U : 0U), (unsigned int)ack_ms,
                    (unsigned int)total_ms, (unsigned int)st,
                    (unsigned int)sle_job_cache_free(),
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available(),
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                    (unsigned int)g_executed_lines,
                    state_name(g_state));
    }
}

static bool job_end_is_idempotent(const sle_job_end_payload_t *end)
{
    if (end == NULL || !sle_job_cache_is_all_received() ||
        end->job_id != sle_job_cache_job_id() ||
        end->total_size != sle_job_cache_total_size() ||
        sle_job_cache_received() < end->total_size) {
        return false;
    }
    if (end->job_crc16 != 0U && sle_job_cache_crc() != end->job_crc16) {
        return false;
    }
    return true;
}

static bool job_end_matches_completed_job(const sle_job_end_payload_t *end)
{
    if (end == NULL || !g_completed_job_valid ||
        end->job_id != g_completed_job_id ||
        end->total_size != g_completed_job_total ||
        g_completed_job_received < g_completed_job_total) {
        return false;
    }
    if (end->job_crc16 != 0U && g_completed_job_crc != end->job_crc16) {
        return false;
    }
    return true;
}

static void handle_job_end(const sle_job_packet_view_t *pkt)
{
    if (pkt->len != sizeof(sle_job_end_payload_t)) {
        osal_printk("[JOB_END_REJECT] reason=bad_len state=%s seq=%u pkt_len=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq, (unsigned int)pkt->len);
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_JOB);
        return;
    }

    sle_job_end_payload_t end;
    memcpy(&end, pkt->payload, sizeof(end));

    if (job_end_is_idempotent(&end)) {
        seq_commit(pkt->seq);
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_OK);
        osal_printk("[JOB_END_DUP] t=%u seq=%u job=%u rx=%u total=%u state=%s\r\n",
                    (unsigned int)uapi_systick_get_ms(),
                    (unsigned int)pkt->seq,
                    (unsigned int)end.job_id,
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_total_size(),
                    state_name(g_state));
        return;
    }

    if (g_state == SLE_JOB_STATE_IDLE && job_end_matches_completed_job(&end)) {
        seq_commit(pkt->seq);
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_OK);
        osal_printk("[JOB_END_LATE_ACK] t=%u seq=%u job=%u total=%u crc=0x%04x "
                    "lines=%u state=%s\r\n",
                    (unsigned int)uapi_systick_get_ms(),
                    (unsigned int)pkt->seq,
                    (unsigned int)end.job_id,
                    (unsigned int)end.total_size,
                    (unsigned int)end.job_crc16,
                    (unsigned int)g_completed_job_lines,
                    state_name(g_state));
        return;
    }

    if (g_state != SLE_JOB_STATE_RECEIVING_JOB && g_state != SLE_JOB_STATE_EXECUTING &&
        g_state != SLE_JOB_STATE_PAUSED) {
        osal_printk("[JOB_END_REJECT] reason=bad_state state=%s seq=%u job=%u total=%u rx=%u\r\n",
                    state_name(g_state), (unsigned int)pkt->seq,
                    (unsigned int)end.job_id, (unsigned int)end.total_size,
                    (unsigned int)sle_job_cache_received());
        send_ack(pkt->type, pkt->seq, SLE_JOB_STATUS_BAD_STATE);
        return;
    }

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
    uint32_t ack_start_ms = (uint32_t)uapi_systick_get_ms();
    send_ack(pkt->type, pkt->seq, st);
    osal_printk("[JOB_END_ACK_SENT] t=%u seq=%u st=%u ack_ms=%u rx=%u total=%u state=%s\r\n",
                (unsigned int)uapi_systick_get_ms(),
                (unsigned int)pkt->seq,
                (unsigned int)st,
                (unsigned int)((uint32_t)uapi_systick_get_ms() - ack_start_ms),
                (unsigned int)sle_job_cache_received(),
                (unsigned int)sle_job_cache_total_size(),
                state_name(g_state));
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
        g_exec_stream_cache_target_watermark = sle_job_cache_available();
        g_data_cum_ack_offset = sle_job_cache_received();
        g_data_cum_ack_ms = (uint32_t)uapi_systick_get_ms();
        osal_printk("[RX_EXEC_DIAG_BEGIN] job=%u mode=manual sample_us=%u "
                    "delay_chunk_us=%u sleep_th_us=%u profile=%u relief_every=%u relief_ms=%u "
                    "timer_th_us=%u timer_tail_us=%u\r\n",
                    (unsigned int)start.job_id,
                    (unsigned int)SLE_JOB_MOTION_SAMPLE_PERIOD_US,
                    (unsigned int)SLE_JOB_MOTION_DELAY_CHUNK_US,
                    (unsigned int)SLE_JOB_MOTION_SLEEP_THRESHOLD_US,
                    (unsigned int)SLE_JOB_MOTION_SPEED_PROFILE,
                    (unsigned int)SLE_JOB_MOTION_SCHED_RELIEF_INTERVAL,
                    (unsigned int)SLE_JOB_MOTION_SCHED_RELIEF_MS,
                    (unsigned int)SLE_JOB_MOTION_TIMER_THRESHOLD_US,
                    (unsigned int)SLE_JOB_MOTION_TIMER_TAIL_US);
        osal_printk("[EXEC_START] stream_target=%u rx=%u consumed=%u avail=%u q=%u\r\n",
                    (unsigned int)g_exec_stream_cache_target_watermark,
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available(),
                    (unsigned int)sle_job_motion_executor_queue_depth());
        g_state = SLE_JOB_STATE_EXECUTING;
        seq_commit(pkt->seq);
    }
    uint32_t ack_start_ms = (uint32_t)uapi_systick_get_ms();
    send_ack(pkt->type, pkt->seq, st);
    osal_printk("[EXEC_START_ACK_SENT] t=%u seq=%u st=%u ack_ms=%u rx=%u consumed=%u avail=%u q=%u state=%s\r\n",
                (unsigned int)uapi_systick_get_ms(),
                (unsigned int)pkt->seq,
                (unsigned int)st,
                (unsigned int)((uint32_t)uapi_systick_get_ms() - ack_start_ms),
                (unsigned int)sle_job_cache_received(),
                (unsigned int)sle_job_cache_consumed(),
                (unsigned int)sle_job_cache_available(),
                (unsigned int)sle_job_motion_executor_queue_depth(),
                state_name(g_state));
    if (st == SLE_JOB_STATUS_OK) {
#if SLE_JOB_EXEC_START_ACK_GRACE_MS > 0
        uint32_t grace_start_ms = (uint32_t)uapi_systick_get_ms();
        osal_printk("[EXEC_START_ACK_GRACE] begin t=%u wait=%u rx=%u consumed=%u avail=%u q=%u motion_busy=%u\r\n",
                    (unsigned int)grace_start_ms,
                    (unsigned int)SLE_JOB_EXEC_START_ACK_GRACE_MS,
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available(),
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U));
        osal_msleep(SLE_JOB_EXEC_START_ACK_GRACE_MS);
        osal_printk("[EXEC_START_ACK_GRACE] end t=%u waited=%u rx=%u consumed=%u avail=%u q=%u motion_busy=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(),
                    (unsigned int)((uint32_t)uapi_systick_get_ms() - grace_start_ms),
                    (unsigned int)sle_job_cache_received(),
                    (unsigned int)sle_job_cache_consumed(),
                    (unsigned int)sle_job_cache_available(),
                    (unsigned int)sle_job_motion_executor_queue_depth(),
                    (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U));
#endif
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
        {
            uint32_t t_status = (uint32_t)uapi_systick_get_ms();
            uint32_t since_data_ms = (g_last_data_rx_ms == 0U) ? 0U : (uint32_t)(t_status - g_last_data_rx_ms);
            uint16_t expected_seq = g_expected_seq;
            uint16_t last_seq = g_last_seq;
            /* STATUS_REQ is out-of-band and must not advance the ordered DATA/control seq. */
            send_status(SLE_JOB_STATUS_OK);
            uint32_t status_ms = (uint32_t)uapi_systick_get_ms() - t_status;
            if (status_ms >= SLE_JOB_SEND_SLOW_MS) {
                osal_printk("[RX_STATUS_REQ] seq=%u send_ms=%u since_data_ms=%u state=%s "
                            "rx=%u consumed=%u avail=%u q=%u motion_busy=%u lines=%u "
                            "expected=%u last=%u seq_neutral=1\r\n",
                            (unsigned int)pkt.seq, (unsigned int)status_ms,
                            (unsigned int)since_data_ms, state_name(g_state),
                            (unsigned int)sle_job_cache_received(),
                            (unsigned int)sle_job_cache_consumed(),
                            (unsigned int)sle_job_cache_available(),
                            (unsigned int)sle_job_motion_executor_queue_depth(),
                            (unsigned int)(sle_job_motion_executor_is_busy() ? 1U : 0U),
                            (unsigned int)g_executed_lines,
                            (unsigned int)expected_seq, (unsigned int)last_seq);
            }
            break;
        }
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
    g_last_data_rx_ms = 0;
    g_exec_stream_cache_target_watermark = 0;
    reset_auto_exec_policy();
}

bool sle_job_manager_is_idle(void)
{
    return g_state == SLE_JOB_STATE_IDLE && !g_exec_active;
}

void sle_job_manager_init(void)
{
    sle_job_cache_init();
    clear_completed_job_summary();
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
    g_last_data_rx_ms = 0;
    g_exec_stream_cache_target_watermark = 0;
    reset_auto_exec_policy();
#if SLE_JOB_PANEL_STATUS_BROADCAST_ENABLE
    g_panel_status_seq = 1;
#endif
    start_panel_status_task_once();
    start_job_exec_task_once();
    start_auto_exec_task_once();
}
