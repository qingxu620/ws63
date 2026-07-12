/**
 * @file sle_job_server.c
 * @brief SLE server for structured laser job packets.
 */
#include "sle_job_route_server.h"
#include "common_def.h"
#include "sle_job_config.h"
#include "errcode.h"
#include "sle_job_protocol.h"
#include "sle_job_motion_executor.h"
#include "securec.h"
#include "soc_osal.h"
#include "systick.h"

#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_server.h"
#include <string.h>

#ifndef SLE_ADV_HANDLE_DEFAULT
#define SLE_ADV_HANDLE_DEFAULT 1
#endif

#ifndef SLE_ADV_CHANNEL_MAP_DEFAULT
#define SLE_ADV_CHANNEL_MAP_DEFAULT 0x07
#endif

#ifndef SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL
#define SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL 0x01
#endif

#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS
#define SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS 0x05
#endif

#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME
#define SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME 0x0B
#endif

#ifndef SLE_ADV_DATA_TYPE_TX_POWER_LEVEL
#define SLE_ADV_DATA_TYPE_TX_POWER_LEVEL 0x0C
#endif

#ifndef SLE_ADV_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA
#define SLE_ADV_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA 0xFF
#endif

#define UUID_LEN_2 2
#define SLE_CONN_INVALID 0xFFFF
#define SLE_CONN_BROADCAST 0xFFFF
#define SLE_JOB_ROUTE_MAX_CONNECTIONS 4U
#define SLE_JOB_ADV_DATA_LEN_MAX 251U
#define SLE_JOB_ADV_TX_POWER 20U
#define SLE_JOB_CONNECTED_ANNOUNCE_ENABLE 0
#define SLE_JOB_RX_WORK_QUEUE_SIZE 8U
#define SLE_JOB_RX_CB_SLOW_MS 5U
#define SLE_JOB_RX_WORK_SLOW_MS 20U
#define SLE_JOB_RX_WORK_USE_SEM 0
#define SLE_JOB_RX_CB_LOG_FIRST 0U
#define SLE_JOB_RX_CB_LOG_EVERY 0U
#define SLE_JOB_RX_CB_GAP_SLOW_MS 250U
#define SLE_JOB_NOTIFY_CALL_SLOW_MS 20U

typedef struct {
    uint16_t conn_id;
    uint16_t len;
    uint32_t serial;
    uint32_t cb_start_ms;
    uint32_t enqueue_ms;
    uint32_t ready_ms;
    uint8_t data[SLE_JOB_PACKET_MAX_SIZE];
} sle_job_rx_work_item_t;

typedef struct {
    bool header_ok;
    bool is_data;
    uint8_t type;
    uint16_t seq;
    uint16_t payload_len;
    uint32_t job_id;
    uint32_t offset;
    uint16_t data_len;
} sle_job_rx_cb_diag_t;

static const uint8_t g_receiver_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x01};
static const uint8_t g_tx_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x03};
static const uint8_t g_screen_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x02};
static volatile uint16_t g_owner_conn_id = SLE_CONN_INVALID;
static volatile uint16_t g_conn_ids[SLE_JOB_ROUTE_MAX_CONNECTIONS];
static volatile bool g_conn_table_ready = false;
static uint8_t g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_data_property_handle = 0;
static uint16_t g_resp_property_handle = 0;
static sle_job_route_packet_rx_cb_t g_packet_cb = NULL;
static sle_job_route_disconnect_cb_t g_disconnect_cb = NULL;
static volatile bool g_server_stopping = false;
static volatile bool g_adv_data_ready = false;
static volatile bool g_adv_enabled = false;
static volatile bool g_adv_restart_pending = false;
static volatile bool g_server_configured = false;
static uint8_t g_scan_rsp_data[SLE_JOB_ADV_DATA_LEN_MAX];
static uint16_t g_scan_rsp_data_len = 0;
static osal_mutex g_rx_work_mutex;
static osal_semaphore g_rx_work_sem;
static bool g_rx_work_sync_ready = false;
static bool g_rx_work_task_started = false;
static volatile bool g_rx_work_accepting = false;
static uint8_t g_rx_work_head = 0;
static uint8_t g_rx_work_tail = 0;
static uint32_t g_rx_work_enqueued = 0;
static uint32_t g_rx_work_dropped = 0;
static uint8_t g_rx_work_max_used = 0;
static uint32_t g_rx_cb_last_owner_ms = 0;
static uint32_t g_rx_cb_data_index = 0;
static sle_job_rx_work_item_t g_rx_work_queue[SLE_JOB_RX_WORK_QUEUE_SIZE];

static uint8_t sle_uuid_base[] = {
    0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void conn_table_reset(void)
{
    for (uint8_t i = 0; i < SLE_JOB_ROUTE_MAX_CONNECTIONS; i++) {
        g_conn_ids[i] = SLE_CONN_INVALID;
    }
    g_owner_conn_id = SLE_CONN_INVALID;
    g_conn_table_ready = true;
}

static bool conn_table_contains(uint16_t conn_id)
{
    if (conn_id == SLE_CONN_INVALID) {
        return false;
    }
    for (uint8_t i = 0; i < SLE_JOB_ROUTE_MAX_CONNECTIONS; i++) {
        if (g_conn_ids[i] == conn_id) {
            return true;
        }
    }
    return false;
}

static uint8_t conn_table_count(void)
{
    if (!g_conn_table_ready) {
        return 0;
    }
    uint8_t count = 0;
    for (uint8_t i = 0; i < SLE_JOB_ROUTE_MAX_CONNECTIONS; i++) {
        if (g_conn_ids[i] != SLE_CONN_INVALID) {
            count++;
        }
    }
    return count;
}

static bool addr_matches_mac(const sle_addr_t *addr, const uint8_t *mac)
{
    if (addr == NULL || mac == NULL) {
        return false;
    }
    return memcmp(addr->addr, mac, SLE_ADDR_LEN) == 0;
}

static uint8_t rx_work_queue_used_locked(void)
{
    if (g_rx_work_head >= g_rx_work_tail) {
        return (uint8_t)(g_rx_work_head - g_rx_work_tail);
    }
    return (uint8_t)(SLE_JOB_RX_WORK_QUEUE_SIZE - g_rx_work_tail + g_rx_work_head);
}

static uint16_t rx_diag_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t rx_diag_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static sle_job_rx_cb_diag_t rx_cb_diag_parse_packet(const uint8_t *data, uint16_t len)
{
    sle_job_rx_cb_diag_t diag = {0};
    if (data == NULL || len < SLE_JOB_PACKET_HEADER_LEN) {
        return diag;
    }

    uint16_t magic = rx_diag_le16(&data[0]);
    if (magic != SLE_JOB_PACKET_MAGIC) {
        return diag;
    }

    diag.header_ok = true;
    diag.type = data[2];
    diag.seq = rx_diag_le16(&data[4]);
    diag.payload_len = rx_diag_le16(&data[6]);

    if (diag.type != SLE_JOB_PKT_JOB_DATA ||
        diag.payload_len < sizeof(sle_job_data_payload_t) ||
        len < (uint16_t)(SLE_JOB_PACKET_HEADER_LEN + sizeof(sle_job_data_payload_t))) {
        return diag;
    }

    const uint8_t *payload = &data[SLE_JOB_PACKET_HEADER_LEN];
    diag.is_data = true;
    diag.job_id = rx_diag_le32(&payload[0]);
    diag.offset = rx_diag_le32(&payload[4]);
    diag.data_len = rx_diag_le16(&payload[8]);
    return diag;
}

static void rx_work_queue_clear_locked(void)
{
    g_rx_work_head = 0;
    g_rx_work_tail = 0;
#if SLE_JOB_RX_WORK_USE_SEM
    while (osal_sem_down_timeout(&g_rx_work_sem, 0) == OSAL_SUCCESS) {
    }
#endif
}

static bool rx_work_queue_push(uint16_t conn_id, const uint8_t *data, uint16_t len, uint32_t cb_start_ms)
{
    if (!g_rx_work_sync_ready || !g_rx_work_accepting || data == NULL || len == 0 ||
        len > SLE_JOB_PACKET_MAX_SIZE) {
        return false;
    }

    sle_job_rx_cb_diag_t diag = rx_cb_diag_parse_packet(data, len);
    uint32_t prev_cb_ms = g_rx_cb_last_owner_ms;
    uint32_t cb_gap_ms = (prev_cb_ms == 0U) ? 0U : (uint32_t)(cb_start_ms - prev_cb_ms);
    g_rx_cb_last_owner_ms = cb_start_ms;
    uint32_t data_index = 0;
    if (diag.is_data) {
        g_rx_cb_data_index++;
        data_index = g_rx_cb_data_index;
    }

    uint32_t t_lock = (uint32_t)uapi_systick_get_ms();
    uint32_t pre_lock_ms = t_lock - cb_start_ms;
    osal_mutex_lock(&g_rx_work_mutex);
    uint32_t lock_ms = (uint32_t)uapi_systick_get_ms() - t_lock;

    uint8_t next = (uint8_t)((g_rx_work_head + 1U) % SLE_JOB_RX_WORK_QUEUE_SIZE);
    if (next == g_rx_work_tail) {
        g_rx_work_dropped++;
        uint8_t used = rx_work_queue_used_locked();
        osal_mutex_unlock(&g_rx_work_mutex);
        osal_printk("[RX_CB_DROP] conn=%u len=%u used=%u drops=%u lock_ms=%u cb_ms=%u\r\n",
                    (unsigned int)conn_id, (unsigned int)len, (unsigned int)used,
                    (unsigned int)g_rx_work_dropped, (unsigned int)lock_ms,
                    (unsigned int)((uint32_t)uapi_systick_get_ms() - cb_start_ms));
        return false;
    }

    uint32_t serial = g_rx_work_enqueued + 1U;
    uint32_t t_copy = (uint32_t)uapi_systick_get_ms();
    sle_job_rx_work_item_t *item = &g_rx_work_queue[g_rx_work_head];
    item->conn_id = conn_id;
    item->len = len;
    item->serial = serial;
    item->cb_start_ms = cb_start_ms;
    item->enqueue_ms = t_copy;
    (void)memcpy_s(item->data, sizeof(item->data), data, len);
    uint32_t copy_ms = (uint32_t)uapi_systick_get_ms() - t_copy;
    uint32_t ready_ms = (uint32_t)uapi_systick_get_ms();
    item->ready_ms = ready_ms;
    g_rx_work_head = next;
    g_rx_work_enqueued++;
    uint8_t used = rx_work_queue_used_locked();
    if (used > g_rx_work_max_used) {
        g_rx_work_max_used = used;
    }
    uint32_t t_unlock = (uint32_t)uapi_systick_get_ms();
    osal_mutex_unlock(&g_rx_work_mutex);
    uint32_t unlock_ms = (uint32_t)uapi_systick_get_ms() - t_unlock;
    uint32_t sem_up_ms = 0;
#if SLE_JOB_RX_WORK_USE_SEM
    uint32_t t_sem = (uint32_t)uapi_systick_get_ms();
    osal_sem_up(&g_rx_work_sem);
    sem_up_ms = (uint32_t)uapi_systick_get_ms() - t_sem;
#endif

    uint32_t cb_ms = (uint32_t)uapi_systick_get_ms() - cb_start_ms;
    bool periodic_log = serial <= SLE_JOB_RX_CB_LOG_FIRST ||
        (SLE_JOB_RX_CB_LOG_EVERY > 0U && (serial % SLE_JOB_RX_CB_LOG_EVERY) == 0U);
    bool data_trace_log = diag.is_data &&
        (data_index <= SLE_JOB_RX_CB_LOG_FIRST ||
         (SLE_JOB_RX_CB_LOG_EVERY > 0U && (data_index % SLE_JOB_RX_CB_LOG_EVERY) == 0U));
    bool cb_gap_slow = cb_gap_ms >= SLE_JOB_RX_CB_GAP_SLOW_MS;
    if (periodic_log || cb_ms >= SLE_JOB_RX_CB_SLOW_MS || pre_lock_ms > 0U || copy_ms > 0U ||
        lock_ms > 0U || unlock_ms > 0U || sem_up_ms > 0U ||
        used > (SLE_JOB_RX_WORK_QUEUE_SIZE / 2U)) {
        osal_printk("[RX_CB_TIMING] conn=%u len=%u serial=%u cb_ms=%u pre_lock_ms=%u "
                    "lock_ms=%u copy_ms=%u unlock_ms=%u sem_up_ms=%u "
                    "type=0x%02x seq=%u payload=%u header=%u data_idx=%u "
                    "job=%u off=%u data_len=%u q_used=%u q_max=%u enq=%u drops=%u wake=%u\r\n",
                    (unsigned int)conn_id, (unsigned int)len,
                    (unsigned int)serial, (unsigned int)cb_ms,
                    (unsigned int)pre_lock_ms, (unsigned int)lock_ms,
                    (unsigned int)copy_ms, (unsigned int)unlock_ms,
                    (unsigned int)sem_up_ms,
                    (unsigned int)diag.type, (unsigned int)diag.seq,
                    (unsigned int)diag.payload_len,
                    (unsigned int)(diag.header_ok ? 1U : 0U),
                    (unsigned int)data_index, (unsigned int)diag.job_id,
                    (unsigned int)diag.offset, (unsigned int)diag.data_len,
                    (unsigned int)used, (unsigned int)g_rx_work_max_used,
                    (unsigned int)g_rx_work_enqueued, (unsigned int)g_rx_work_dropped,
                    (unsigned int)SLE_JOB_RX_WORK_USE_SEM);
    }
    if (cb_gap_slow || data_trace_log) {
        bool motion_busy = false;
        uint16_t motion_q = 0;
        unsigned long motion_enq = 0;
        unsigned long motion_exec = 0;
        unsigned long motion_late = 0;
        unsigned long motion_missed = 0;
        unsigned long motion_max_late = 0;
        unsigned long motion_last_activity = 0;
        if (cb_gap_slow) {
            motion_busy = sle_job_motion_executor_is_busy();
            motion_q = sle_job_motion_executor_queue_depth();
            motion_enq = sle_job_motion_executor_enqueued_count();
            motion_exec = sle_job_motion_executor_executed_count();
            motion_late = sle_job_motion_executor_late_sample_count();
            motion_missed = sle_job_motion_executor_missed_sample_count();
            motion_max_late = sle_job_motion_executor_max_sample_late_us();
            motion_last_activity = sle_job_motion_executor_last_activity_ms();
        }
        osal_printk("%s conn=%u len=%u serial=%u cb_gap_ms=%u cb_ms=%u "
                    "type=0x%02x seq=%u payload=%u header=%u data_idx=%u "
                    "job=%u off=%u data_len=%u q_used=%u q_max=%u drops=%u wake=%u "
                    "motion_busy=%u motion_q=%u motion_enq=%lu motion_exec=%lu "
                    "motion_late=%lu motion_missed=%lu motion_max_late_us=%lu motion_last=%lu\r\n",
                    cb_gap_slow ? "[RX_CB_SLOW]" : "[RX_CB_TRACE]",
                    (unsigned int)conn_id, (unsigned int)len,
                    (unsigned int)serial, (unsigned int)cb_gap_ms,
                    (unsigned int)cb_ms, (unsigned int)diag.type,
                    (unsigned int)diag.seq, (unsigned int)diag.payload_len,
                    (unsigned int)(diag.header_ok ? 1U : 0U),
                    (unsigned int)data_index, (unsigned int)diag.job_id,
                    (unsigned int)diag.offset, (unsigned int)diag.data_len,
                    (unsigned int)used, (unsigned int)g_rx_work_max_used,
                    (unsigned int)g_rx_work_dropped,
                    (unsigned int)SLE_JOB_RX_WORK_USE_SEM,
                    (unsigned int)(motion_busy ? 1U : 0U),
                    (unsigned int)motion_q,
                    motion_enq,
                    motion_exec,
                    motion_late,
                    motion_missed,
                    motion_max_late,
                    motion_last_activity);
    }
    return true;
}

static bool rx_work_queue_pop(sle_job_rx_work_item_t *out)
{
    if (!g_rx_work_sync_ready || out == NULL) {
        return false;
    }
#if SLE_JOB_RX_WORK_USE_SEM
    if (osal_sem_down(&g_rx_work_sem) != OSAL_SUCCESS) {
        return false;
    }
#endif

    osal_mutex_lock(&g_rx_work_mutex);
    if (g_rx_work_head == g_rx_work_tail) {
        osal_mutex_unlock(&g_rx_work_mutex);
        return false;
    }
    (void)memcpy_s(out, sizeof(*out), &g_rx_work_queue[g_rx_work_tail], sizeof(*out));
    g_rx_work_tail = (uint8_t)((g_rx_work_tail + 1U) % SLE_JOB_RX_WORK_QUEUE_SIZE);
    osal_mutex_unlock(&g_rx_work_mutex);
    return true;
}

static uint8_t rx_work_queue_used(void);

static int rx_work_task(void *arg)
{
    unused(arg);
    sle_job_rx_work_item_t item;

    while (1) {
        if (!rx_work_queue_pop(&item)) {
            osal_msleep(1);
            continue;
        }

        uint32_t t_start = (uint32_t)uapi_systick_get_ms();
        uint32_t wait_ms = t_start - item.enqueue_ms;
        uint32_t cb_to_work_ms = t_start - item.cb_start_ms;
        uint32_t ready_to_work_ms = t_start - item.ready_ms;
        sle_job_rx_cb_diag_t diag = rx_cb_diag_parse_packet(item.data, item.len);
        if (item.conn_id == g_owner_conn_id && g_packet_cb != NULL) {
            g_packet_cb(item.conn_id, item.data, item.len);
        } else {
            osal_printk("[RX_WORK_DROP] conn=%u owner=%u len=%u wait_ms=%u cb=%u\r\n",
                        (unsigned int)item.conn_id, (unsigned int)g_owner_conn_id,
                        (unsigned int)item.len, (unsigned int)wait_ms,
                        (unsigned int)(g_packet_cb != NULL));
        }
        uint32_t proc_ms = (uint32_t)uapi_systick_get_ms() - t_start;
        if (wait_ms >= SLE_JOB_RX_WORK_SLOW_MS || proc_ms >= SLE_JOB_RX_WORK_SLOW_MS ||
            cb_to_work_ms >= SLE_JOB_RX_WORK_SLOW_MS ||
            ready_to_work_ms >= SLE_JOB_RX_WORK_SLOW_MS) {
            osal_printk("[RX_WORK_TIMING] conn=%u len=%u serial=%u wait_ms=%u "
                        "ready_to_work_ms=%u cb_to_work_ms=%u proc_ms=%u "
                        "type=0x%02x seq=%u payload=%u header=%u "
                        "job=%u off=%u data_len=%u owner=%u q_used=%u\r\n",
                        (unsigned int)item.conn_id, (unsigned int)item.len,
                        (unsigned int)item.serial, (unsigned int)wait_ms,
                        (unsigned int)ready_to_work_ms,
                        (unsigned int)cb_to_work_ms, (unsigned int)proc_ms,
                        (unsigned int)diag.type,
                        (unsigned int)diag.seq,
                        (unsigned int)diag.payload_len,
                        (unsigned int)(diag.header_ok ? 1U : 0U),
                        (unsigned int)diag.job_id,
                        (unsigned int)diag.offset,
                        (unsigned int)diag.data_len,
                        (unsigned int)g_owner_conn_id,
                        (unsigned int)rx_work_queue_used());
        }
    }

    return 0;
}

static errcode_t rx_work_queue_start(void)
{
    if (!g_rx_work_sync_ready) {
        if (osal_mutex_init(&g_rx_work_mutex) != OSAL_SUCCESS ||
            osal_sem_init(&g_rx_work_sem, 0) != OSAL_SUCCESS) {
            osal_printk("[RX_WORK] sync init failed\r\n");
            return ERRCODE_FAIL;
        }
        g_rx_work_sync_ready = true;
    }

    osal_mutex_lock(&g_rx_work_mutex);
    rx_work_queue_clear_locked();
    g_rx_work_accepting = true;
    g_rx_work_enqueued = 0;
    g_rx_work_dropped = 0;
    g_rx_work_max_used = 0;
    g_rx_cb_last_owner_ms = 0;
    g_rx_cb_data_index = 0;
    osal_mutex_unlock(&g_rx_work_mutex);

    if (g_rx_work_task_started) {
        return ERRCODE_SUCC;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(rx_work_task, NULL, "sle_rx_work", SLE_JOB_TASK_STACK_SIZE_SLE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_mutex_lock(&g_rx_work_mutex);
        g_rx_work_accepting = false;
        osal_mutex_unlock(&g_rx_work_mutex);
        osal_printk("[RX_WORK] create task failed\r\n");
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_TASK_PRIO_SLE) != OSAL_SUCCESS) {
        osal_printk("[RX_WORK] set priority failed\r\n");
    }
    osal_kfree(task);
    g_rx_work_task_started = true;
    osal_kthread_unlock();
    osal_printk("[RX_WORK] started queue=%u prio=%u wake=%u\r\n",
                (unsigned int)SLE_JOB_RX_WORK_QUEUE_SIZE,
                (unsigned int)SLE_JOB_TASK_PRIO_SLE,
                (unsigned int)SLE_JOB_RX_WORK_USE_SEM);
    return ERRCODE_SUCC;
}

static void rx_work_queue_stop(void)
{
    if (!g_rx_work_sync_ready) {
        return;
    }
    osal_mutex_lock(&g_rx_work_mutex);
    g_rx_work_accepting = false;
    rx_work_queue_clear_locked();
    osal_mutex_unlock(&g_rx_work_mutex);
}

static void rx_work_queue_clear(void)
{
    if (!g_rx_work_sync_ready) {
        return;
    }
    osal_mutex_lock(&g_rx_work_mutex);
    rx_work_queue_clear_locked();
    osal_mutex_unlock(&g_rx_work_mutex);
}

static uint8_t rx_work_queue_used(void)
{
    if (!g_rx_work_sync_ready) {
        return 0;
    }
    osal_mutex_lock(&g_rx_work_mutex);
    uint8_t used = rx_work_queue_used_locked();
    osal_mutex_unlock(&g_rx_work_mutex);
    return used;
}

static void conn_table_add(uint16_t conn_id)
{
    if (conn_id == SLE_CONN_INVALID || conn_table_contains(conn_id)) {
        return;
    }
    for (uint8_t i = 0; i < SLE_JOB_ROUTE_MAX_CONNECTIONS; i++) {
        if (g_conn_ids[i] == SLE_CONN_INVALID) {
            g_conn_ids[i] = conn_id;
            return;
        }
    }
    osal_printk("[job_rx] conn table full, observer conn_id=%u not tracked\r\n", conn_id);
}

static bool conn_table_remove(uint16_t conn_id)
{
    bool removed = false;
    for (uint8_t i = 0; i < SLE_JOB_ROUTE_MAX_CONNECTIONS; i++) {
        if (g_conn_ids[i] == conn_id) {
            g_conn_ids[i] = SLE_CONN_INVALID;
            removed = true;
        }
    }
    return removed;
}

static void sle_uuid_set_base(sle_uuid_t *out)
{
    (void)memcpy_s(out->uuid, SLE_UUID_LEN, sle_uuid_base, SLE_UUID_LEN);
    out->len = UUID_LEN_2;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    sle_uuid_set_base(out);
    out->uuid[14] = (uint8_t)(u2 & 0xFF);
    out->uuid[15] = (uint8_t)((u2 >> 8) & 0xFF);
}

static void ssaps_write_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    uint32_t t_cb = (uint32_t)uapi_systick_get_ms();
    if (status != ERRCODE_SLE_SUCCESS || write_cb_para == NULL ||
        write_cb_para->value == NULL || write_cb_para->length == 0) {
        return;
    }
    if (write_cb_para->length > SLE_JOB_PACKET_MAX_SIZE) {
        osal_printk("[RX_CB_DROP] reason=too_large conn=%u len=%u max=%u\r\n",
                    (unsigned int)conn_id, (unsigned int)write_cb_para->length,
                    (unsigned int)SLE_JOB_PACKET_MAX_SIZE);
        return;
    }
    if (!conn_table_contains(conn_id)) {
        osal_printk("[RX_CB_DROP] reason=non_whitelist_conn conn=%u len=%u\r\n",
                    (unsigned int)conn_id, (unsigned int)write_cb_para->length);
        return;
    }

    if (g_owner_conn_id == SLE_CONN_INVALID) {
        g_owner_conn_id = conn_id;
    }

    if (conn_id != g_owner_conn_id) {
        osal_printk("[job_rx] drop non-owner write conn_id=%u owner=%u len=%u\r\n",
                    conn_id, g_owner_conn_id, write_cb_para->length);
        return;
    }

    if (g_packet_cb != NULL) {
        if (!rx_work_queue_push(conn_id, write_cb_para->value, write_cb_para->length, t_cb)) {
            osal_printk("[RX_CB_DROP] reason=enqueue_fail conn=%u len=%u accepting=%u cb=%u\r\n",
                        (unsigned int)conn_id, (unsigned int)write_cb_para->length,
                        (unsigned int)g_rx_work_accepting,
                        (unsigned int)(g_packet_cb != NULL));
        }
    }
}

static void ssaps_read_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_read_cb_t *read_cb_para, errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(read_cb_para);
    unused(status);
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,
    ssap_exchange_info_t *mtu_size, errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(status);
    unused(mtu_size);
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(handle);
    unused(status);
}

static void sle_ssaps_register_cbks(void)
{
    ssaps_callbacks_t cbk = {0};
    cbk.start_service_cb = ssaps_start_service_cbk;
    cbk.mtu_changed_cb = ssaps_mtu_changed_cbk;
    cbk.read_request_cb = ssaps_read_request_cbk;
    cbk.write_request_cb = ssaps_write_request_cbk;
    ssaps_register_callbacks(&cbk);
}

static errcode_t sle_job_add_service(void)
{
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_JOB_SERVICE_UUID, &service_uuid);
    return ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
}

static errcode_t sle_job_add_data_property(void)
{
    ssaps_property_info_t property = {0};
    uint8_t init_val[SLE_JOB_PACKET_MAX_SIZE] = {0};

    property.permissions = 0x01 | 0x02;
    sle_uuid_setu2(SLE_JOB_DATA_CHAR_UUID, &property.uuid);
    property.value = init_val;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_WRITE | SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP;
    return ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_data_property_handle);
}

static errcode_t sle_job_add_resp_property(void)
{
    ssaps_property_info_t property = {0};
    uint8_t init_val[SLE_JOB_PACKET_MAX_SIZE] = {0};
    uint8_t ntf_value[] = {0x01, 0x00};

    property.permissions = 0x01 | 0x02;
    sle_uuid_setu2(SLE_JOB_RESP_CHAR_UUID, &property.uuid);
    property.value = init_val;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY;

    errcode_t ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_resp_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    ssaps_desc_info_t desc = {0};
    desc.permissions = 0x01 | 0x02;
    desc.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    desc.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    desc.value = ntf_value;
    desc.value_len = sizeof(ntf_value);
    return ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_resp_property_handle, &desc);
}

static errcode_t sle_job_route_server_add(void)
{
    sle_uuid_t app_uuid = {0};
    char app_uuid_data[] = {0x0, 0x0};
    app_uuid.len = sizeof(app_uuid_data);
    (void)memcpy_s(app_uuid.uuid, app_uuid.len, app_uuid_data, sizeof(app_uuid_data));

    errcode_t ret = ssaps_register_server(&app_uuid, &g_server_id);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] register server fail: 0x%x\r\n", ret);
        return ret;
    }
    if (sle_job_add_service() != ERRCODE_SLE_SUCCESS ||
        sle_job_add_data_property() != ERRCODE_SLE_SUCCESS ||
        sle_job_add_resp_property() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] add service/properties fail\r\n");
        return ERRCODE_SLE_FAIL;
    }

    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] start service fail: 0x%x\r\n", ret);
        return ret;
    }
    return ERRCODE_SLE_SUCCESS;
}

static void tune_job_link_after_connect(uint16_t conn_id)
{
#if SLE_JOB_LINK_DATA_LEN_ENABLE
    errcode_t ret = sle_set_data_len(conn_id, SLE_JOB_LINK_DATA_LEN_OCTETS);
    osal_printk("[job_rx_link_tune] conn=%u data_len=%u ret=0x%x\r\n",
                (unsigned int)conn_id,
                (unsigned int)SLE_JOB_LINK_DATA_LEN_OCTETS,
                (unsigned int)ret);
#endif

#if SLE_JOB_LINK_HIGH_THROUGHPUT_ENABLE
    sle_set_phy_t phy_param = {
        .tx_format = SLE_RADIO_FRAME_2,
        .rx_format = SLE_RADIO_FRAME_2,
        .tx_phy = SLE_PHY_4M,
        .rx_phy = SLE_PHY_4M,
        .tx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .rx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .g_feedback = 0,
        .t_feedback = 0,
    };
    errcode_t phy_ret = sle_set_phy_param(conn_id, &phy_param);
    osal_printk("[job_rx_link_phy] conn=%u frame=%u phy=%u pilot=%u ret=0x%x\r\n",
                (unsigned int)conn_id,
                (unsigned int)SLE_RADIO_FRAME_2,
                (unsigned int)SLE_PHY_4M,
                (unsigned int)SLE_PHY_PILOT_DENSITY_16_TO_1,
                (unsigned int)phy_ret);
#endif
}

#if SLE_JOB_LINK_HIGH_THROUGHPUT_ENABLE
static void sle_set_phy_cbk(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param)
{
    uint8_t tx_phy = (param != NULL) ? param->tx_phy : 0xFF;
    uint8_t rx_phy = (param != NULL) ? param->rx_phy : 0xFF;
    errcode_t mcs_ret = ERRCODE_SLE_FAIL;
    if (status == ERRCODE_SLE_SUCCESS) {
        mcs_ret = sle_set_mcs(conn_id, SLE_MCS_10);
    }
    osal_printk("[job_rx_link_phy_cb] conn=%u status=0x%x tx_phy=%u rx_phy=%u "
                "mcs=%u mcs_ret=0x%x\r\n",
                (unsigned int)conn_id,
                (unsigned int)status,
                (unsigned int)tx_phy,
                (unsigned int)rx_phy,
                (unsigned int)SLE_MCS_10,
                (unsigned int)mcs_ret);
}
#endif

static void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        bool is_tx = addr_matches_mac(addr, g_tx_mac);
        bool is_screen = addr_matches_mac(addr, g_screen_mac);
        if (!is_tx && !is_screen) {
            osal_printk("[job_rx] reject non-whitelist peer conn_id=%u\r\n", (unsigned int)conn_id);
            if (addr != NULL) {
                (void)sle_disconnect_remote_device(addr);
            }
            return;
        }
        osal_printk("[job_rx] accept fixed %s peer conn_id=%u\r\n",
                    is_tx ? "TX" : "Screen", (unsigned int)conn_id);
        conn_table_add(conn_id);
        tune_job_link_after_connect(conn_id);
#if SLE_JOB_CONNECTED_ANNOUNCE_ENABLE
        errcode_t adv_ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
        unused(adv_ret);
#else
        if (g_adv_enabled) {
            errcode_t adv_ret = sle_stop_announce(SLE_ADV_HANDLE_DEFAULT);
            if (adv_ret == ERRCODE_SLE_SUCCESS) {
                osal_printk("[job_rx] connected stop announce conn_id=%u\r\n", conn_id);
            } else {
                osal_printk("[job_rx] connected stop announce fail: 0x%x\r\n", adv_ret);
            }
        }
#endif
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        bool was_owner = (conn_id == g_owner_conn_id);
        (void)conn_table_remove(conn_id);
        if (was_owner) {
            g_owner_conn_id = SLE_CONN_INVALID;
            rx_work_queue_clear();
        }
        if (g_server_stopping) {
            return;
        }
        if (was_owner && g_disconnect_cb != NULL) {
            osal_printk("[job_rx] owner disconnected, force safe stop\r\n");
            g_disconnect_cb();
        }
        if (conn_table_count() == 0U) {
            sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
        }
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    unused(status);
}

static void sle_auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr,
    errcode_t status, const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    unused(status);
}

static void sle_conn_register_cbks(void)
{
    sle_connection_callbacks_t cbk = {0};
    cbk.connect_state_changed_cb = sle_connect_state_changed_cbk;
    cbk.pair_complete_cb = sle_pair_complete_cbk;
    cbk.auth_complete_cb = sle_auth_complete_cbk;
#if SLE_JOB_LINK_HIGH_THROUGHPUT_ENABLE
    cbk.set_phy_cb = sle_set_phy_cbk;
#endif
    sle_connection_register_callbacks(&cbk);
}

static uint8_t g_announce_data[SLE_JOB_ADV_DATA_LEN_MAX];
static uint16_t g_announce_data_len;

static bool adv_append_field(uint8_t *buf, uint16_t max_len, uint16_t *idx,
                             uint8_t type, const void *data, uint8_t len)
{
    if (buf == NULL || idx == NULL || data == NULL ||
        (uint16_t)(*idx + 2U + len) > max_len) {
        return false;
    }
    buf[(*idx)++] = type;
    buf[(*idx)++] = len;
    (void)memcpy_s(&buf[*idx], (size_t)(max_len - *idx), data, len);
    *idx = (uint16_t)(*idx + len);
    return true;
}

static uint16_t build_scan_rsp_data(const sle_job_panel_status_payload_t *status)
{
    uint16_t idx = 0;
    const uint8_t tx_power = SLE_JOB_ADV_TX_POWER;
    const char name[] = SLE_JOB_RECEIVER_NAME;

    memset(g_scan_rsp_data, 0, sizeof(g_scan_rsp_data));
    if (!adv_append_field(g_scan_rsp_data, sizeof(g_scan_rsp_data), &idx,
                          SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
                          &tx_power, sizeof(tx_power))) {
        return idx;
    }
    if (!adv_append_field(g_scan_rsp_data, sizeof(g_scan_rsp_data), &idx,
                          SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME,
                          name, (uint8_t)(sizeof(name) - 1U))) {
        return idx;
    }
    if (status != NULL) {
        sle_job_panel_status_adv_payload_t adv = {
            .magic0 = SLE_JOB_PANEL_STATUS_ADV_MAGIC0,
            .magic1 = SLE_JOB_PANEL_STATUS_ADV_MAGIC1,
            .version = SLE_JOB_PANEL_STATUS_ADV_VERSION,
            .status = *status,
        };
        (void)adv_append_field(g_scan_rsp_data, sizeof(g_scan_rsp_data), &idx,
                               SLE_ADV_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA,
                               &adv, (uint8_t)sizeof(adv));
    }
    return idx;
}

static uint16_t build_announce_data(const sle_job_panel_status_payload_t *status)
{
    uint16_t idx = 0;
    const uint8_t discovery_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    const uint8_t uuid16[] = {
        (uint8_t)(SLE_JOB_SERVICE_UUID & 0xFF),
        (uint8_t)(SLE_JOB_SERVICE_UUID >> 8),
    };

    memset(g_announce_data, 0, sizeof(g_announce_data));
    if (!adv_append_field(g_announce_data, sizeof(g_announce_data), &idx,
                          SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
                          &discovery_level, sizeof(discovery_level))) {
        return idx;
    }
    if (!adv_append_field(g_announce_data, sizeof(g_announce_data), &idx,
                          SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS,
                          uuid16, sizeof(uuid16))) {
        return idx;
    }
    if (status != NULL) {
        sle_job_panel_status_adv_payload_t adv = {
            .magic0 = SLE_JOB_PANEL_STATUS_ADV_MAGIC0,
            .magic1 = SLE_JOB_PANEL_STATUS_ADV_MAGIC1,
            .version = SLE_JOB_PANEL_STATUS_ADV_VERSION,
            .status = *status,
        };
        (void)adv_append_field(g_announce_data, sizeof(g_announce_data), &idx,
                               SLE_ADV_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA,
                               &adv, (uint8_t)sizeof(adv));
    }
    return idx;
}

static errcode_t set_current_announce_data(void)
{
    sle_announce_data_t data = {0};
    data.announce_data = g_announce_data;
    data.announce_data_len = g_announce_data_len;
    data.seek_rsp_data = g_scan_rsp_data;
    data.seek_rsp_data_len = g_scan_rsp_data_len;
    return sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);
}

static void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] adv enable fail status=0x%02x\r\n", status);
        g_adv_enabled = false;
        return;
    }
    g_adv_enabled = true;
}

static void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] adv disable fail status=0x%02x\r\n", status);
        return;
    }
    g_adv_enabled = false;
    if (g_adv_restart_pending && !g_server_stopping) {
        g_adv_restart_pending = false;
        errcode_t ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
        if (ret != ERRCODE_SLE_SUCCESS) {
            osal_printk("[PANEL_STATUS_ADV] restart fail ret=0x%x\r\n", ret);
        }
    }
}

static void sle_announce_terminal_cbk(uint32_t announce_id)
{
    unused(announce_id);
}

static void sle_enable_cbk(errcode_t status)
{
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] enable fail status=0x%02x\r\n", status);
        return;
    }

    errcode_t ret = sle_job_route_server_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return;
    }

    ssap_exchange_info_t info = {0};
    info.mtu_size = SLE_JOB_MTU_SIZE;
    info.version = 1;
    ret = ssaps_set_info(g_server_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] set ssap info fail: 0x%x\r\n", ret);
        return;
    }

    sle_addr_t addr = {0};
    addr.type = 0;
    memcpy_s(addr.addr, SLE_ADDR_LEN, g_receiver_mac, SLE_ADDR_LEN);
    sle_set_local_addr(&addr);

    sle_announce_param_t param = {0};
    param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
    param.announce_handle = SLE_ADV_HANDLE_DEFAULT;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = 0xC8;
    param.announce_interval_max = 0xC8;
    param.conn_interval_min = SLE_JOB_CONN_INTERVAL_UNITS;
    param.conn_interval_max = SLE_JOB_CONN_INTERVAL_UNITS;
    param.conn_max_latency = 0;
    param.conn_supervision_timeout = 0x1F4;
    param.announce_tx_power = 20;
    param.own_addr = addr;
    osal_printk("[job_rx] sle mtu=%u conn_interval_units=0x%02x\r\n",
                (unsigned int)SLE_JOB_MTU_SIZE,
                (unsigned int)SLE_JOB_CONN_INTERVAL_UNITS);
    ret = sle_set_announce_param(SLE_ADV_HANDLE_DEFAULT, &param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] set adv param fail: 0x%x\r\n", ret);
        return;
    }

    g_announce_data_len = build_announce_data(NULL);
    g_scan_rsp_data_len = build_scan_rsp_data(NULL);
    ret = set_current_announce_data();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] set adv data fail: 0x%x\r\n", ret);
        return;
    }
    g_adv_data_ready = true;

    ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] start announce fail: 0x%x\r\n", ret);
        return;
    }
    g_server_configured = true;
}

static void sle_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t seek_cbks = {0};
    seek_cbks.announce_enable_cb = sle_announce_enable_cbk;
    seek_cbks.announce_disable_cb = sle_announce_disable_cbk;
    seek_cbks.announce_terminal_cb = sle_announce_terminal_cbk;
    seek_cbks.sle_enable_cb = sle_enable_cbk;
    sle_announce_seek_register_callbacks(&seek_cbks);
}

errcode_t sle_job_route_server_init(void)
{
    conn_table_reset();
    g_server_stopping = false;
    if (rx_work_queue_start() != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }
    sle_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();

    if (g_server_configured) {
        g_adv_data_ready = true;
        errcode_t ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
        if (ret != ERRCODE_SLE_SUCCESS) {
            osal_printk("[job_rx] resume announce fail: 0x%x\r\n", ret);
        }
        return ret;
    }

    errcode_t ret = enable_sle();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] enable_sle fail: 0x%x\r\n", ret);
    }
    return ret;
}

errcode_t sle_job_route_server_stop(void)
{
    g_server_stopping = true;
    rx_work_queue_stop();
    if (conn_table_count() > 0U) {
        errcode_t disc_ret = sle_disconnect_all_remote_device();
        if (disc_ret != ERRCODE_SLE_SUCCESS) {
            osal_printk("[job_rx] disconnect all fail: 0x%x\r\n", disc_ret);
        }
        conn_table_reset();
    }
    errcode_t adv_ret = sle_stop_announce(SLE_ADV_HANDLE_DEFAULT);
    if (adv_ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] stop announce fail: 0x%x\r\n", adv_ret);
    }
    g_adv_enabled = false;
    g_adv_restart_pending = false;
    return ERRCODE_SLE_SUCCESS;
}

bool sle_job_route_server_is_connected(void)
{
    return g_owner_conn_id != SLE_CONN_INVALID;
}

errcode_t sle_job_route_server_send_packet(const void *data, uint16_t len)
{
    if (data == NULL || len == 0 || g_owner_conn_id == SLE_CONN_INVALID || g_resp_property_handle == 0) {
        return ERRCODE_SLE_FAIL;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t pkt_type = 0;
    uint16_t pkt_seq = 0;
    uint16_t payload_len = 0;
    bool ack_packet = false;
    uint8_t ack_type = 0;
    uint8_t ack_status = 0;
    uint16_t ack_seq = 0;
    uint32_t ack_offset = 0;
    uint32_t ack_credit = 0;
    if (len >= SLE_JOB_PACKET_HEADER_LEN && rx_diag_le16(&bytes[0]) == SLE_JOB_PACKET_MAGIC) {
        pkt_type = bytes[2];
        pkt_seq = rx_diag_le16(&bytes[4]);
        payload_len = rx_diag_le16(&bytes[6]);
        if ((pkt_type == SLE_JOB_PKT_ACK || pkt_type == SLE_JOB_PKT_NACK) &&
            payload_len == sizeof(sle_job_ack_payload_t) &&
            len >= (uint16_t)(SLE_JOB_PACKET_HEADER_LEN + sizeof(sle_job_ack_payload_t))) {
            const uint8_t *payload = &bytes[SLE_JOB_PACKET_HEADER_LEN];
            ack_packet = true;
            ack_type = payload[0];
            ack_status = payload[1];
            ack_seq = rx_diag_le16(&payload[2]);
            ack_offset = rx_diag_le32(&payload[8]);
            ack_credit = rx_diag_le32(&payload[12]);
        }
    }

    ssaps_ntf_ind_t param = {0};
    param.handle = g_resp_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value_len = len;
    param.value = (uint8_t *)data;
    uint32_t t_notify = (uint32_t)uapi_systick_get_ms();
    errcode_t ret = ssaps_notify_indicate(g_server_id, g_owner_conn_id, &param);
    uint32_t call_ms = (uint32_t)uapi_systick_get_ms() - t_notify;
    if ((ack_packet && (ack_type != SLE_JOB_PKT_JOB_DATA || ack_status != SLE_JOB_STATUS_OK)) ||
        ret != ERRCODE_SLE_SUCCESS || call_ms >= SLE_JOB_NOTIFY_CALL_SLOW_MS) {
        osal_printk("[RX_NOTIFY_TRACE] t=%u pkt=0x%02x pkt_seq=%u ack=%u ack_type=0x%02x "
                    "ack_seq=%u st=%u off=%u credit=%u len=%u ret=0x%x call_ms=%u conn=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(),
                    (unsigned int)pkt_type,
                    (unsigned int)pkt_seq,
                    (unsigned int)(ack_packet ? 1U : 0U),
                    (unsigned int)ack_type,
                    (unsigned int)ack_seq,
                    (unsigned int)ack_status,
                    (unsigned int)ack_offset,
                    (unsigned int)ack_credit,
                    (unsigned int)len,
                    (unsigned int)ret,
                    (unsigned int)call_ms,
                    (unsigned int)g_owner_conn_id);
    }
    return ret;
}

errcode_t sle_job_route_server_broadcast_packet(const void *data, uint16_t len)
{
    if (data == NULL || len == 0 || conn_table_count() == 0U || g_resp_property_handle == 0) {
        return ERRCODE_SLE_FAIL;
    }

    ssaps_ntf_ind_t param = {0};
    param.handle = g_resp_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value_len = len;
    param.value = (uint8_t *)data;
    return ssaps_notify_indicate(g_server_id, SLE_CONN_BROADCAST, &param);
}

errcode_t sle_job_route_server_update_panel_status_adv(const sle_job_panel_status_payload_t *status)
{
    if (!g_adv_data_ready || status == NULL) {
        return ERRCODE_SLE_FAIL;
    }
    g_announce_data_len = build_announce_data(status);
    g_scan_rsp_data_len = build_scan_rsp_data(status);
    errcode_t ret = set_current_announce_data();
    if (ret != ERRCODE_SLE_SUCCESS) {
        static uint32_t s_adv_update_fail_count = 0;
        if ((s_adv_update_fail_count++ & 0x0FU) == 0U) {
            osal_printk("[PANEL_STATUS_ADV] update fail ret=0x%x adv=%u rsp=%u\r\n",
                        ret, (unsigned int)g_announce_data_len,
                        (unsigned int)g_scan_rsp_data_len);
        }
        return ret;
    }
    if (g_adv_enabled) {
        g_adv_restart_pending = true;
        ret = sle_stop_announce(SLE_ADV_HANDLE_DEFAULT);
        if (ret != ERRCODE_SLE_SUCCESS) {
            g_adv_restart_pending = false;
            static uint32_t s_adv_restart_fail_count = 0;
            if ((s_adv_restart_fail_count++ & 0x0FU) == 0U) {
                osal_printk("[PANEL_STATUS_ADV] stop-for-restart fail ret=0x%x\r\n", ret);
            }
        }
    }
    return ret;
}

uint16_t sle_job_route_server_get_owner_conn_id(void)
{
    return g_owner_conn_id;
}

uint8_t sle_job_route_server_get_connection_count(void)
{
    return conn_table_count();
}

const char *sle_job_route_server_get_status(void)
{
    return (conn_table_count() > 0U) ? "connected" : "advertising";
}

void sle_job_route_server_set_packet_cb(sle_job_route_packet_rx_cb_t cb)
{
    g_packet_cb = cb;
}

void sle_job_route_server_set_disconnect_cb(sle_job_route_disconnect_cb_t cb)
{
    g_disconnect_cb = cb;
}
