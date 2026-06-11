/**
 * @file sle_client.c
 * @brief Minimal SLE client for the wireless laser transmitter.
 */
#include "sle_client.h"
#include "config.h"
#include "gcode_processor.h"
#include "protocol.h"
#include "wireless_crc16.h"

#include "common_def.h"
#include "securec.h"
#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"
#include "soc_osal.h"
#include "systick.h"
#include <string.h>

#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME
#define SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME 0x0B
#endif

#ifndef SLE_SEEK_ACTIVE
#define SLE_SEEK_ACTIVE 0x01
#endif

#define SLE_CLIENT_ID_DEFAULT 0
#define SLE_CONN_INVALID 0xFFFF

static uint16_t g_conn_id = SLE_CONN_INVALID;
static bool g_seek_active = false;
static bool g_connect_pending = false;
static sle_addr_t g_pending_addr = {0};
static bool g_handles_ready = false;
static bool g_status_rx_seen = false;
static uint16_t g_cmd_handle = 0;
static uint16_t g_status_handle = 0;
static volatile uint16_t g_pending_writes = 0;
static volatile uint32_t g_last_business_write_ms = 0;
static volatile uint8_t g_remote_status = STATUS_IDLE;
static volatile uint8_t g_queue_free = 0;
static volatile uint16_t g_last_ack_seq = 0;
static double g_remote_x = 0.0;
static double g_remote_y = 0.0;
static volatile uint32_t g_feedback_seq = 0;
static uint32_t g_last_notify_log_ms = 0;
static uint32_t g_notify_rx_count = 0;
static uint32_t g_write_req_count = 0;
static uint32_t g_write_cfm_ok_count = 0;
static uint32_t g_write_cfm_fail_count = 0;
static uint32_t g_write_submit_fail_count = 0;
static uint32_t g_business_write_req_count = 0;
static osal_semaphore g_ack_sem;
static osal_semaphore g_write_sem;
static bool g_ack_sem_ready = false;
static bool g_write_sem_ready = false;

static uint8_t g_target_addr[SLE_ADDR_LEN] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

static bool uuid16_equals(const sle_uuid_t *uuid, uint16_t expect)
{
    return (uuid != NULL) && (uuid->len == 2) && (uuid->uuid[14] == (uint8_t)(expect & 0xFF)) &&
           (uuid->uuid[15] == (uint8_t)((expect >> 8) & 0xFF));
}

static bool seq_reached(uint16_t ack_seq, uint16_t target_seq)
{
    return (uint16_t)(ack_seq - target_seq) < 0x8000U;
}

static bool cmd_requires_write_cfm(const motion_cmd_t *cmd)
{
    return cmd != NULL && (cmd->cmd == CMD_LASER_OFF || cmd->cmd == CMD_EMERGENCY_STOP);
}

static void reset_link_runtime(void)
{
    g_handles_ready = false;
    g_status_rx_seen = false;
    g_cmd_handle = 0;
    g_status_handle = 0;
    {
        unsigned int irq_state = osal_irq_lock();
        g_pending_writes = 0;
        osal_irq_restore(irq_state);
    }
    g_last_business_write_ms = 0;
    g_remote_status = STATUS_IDLE;
    g_queue_free = 0;
    g_last_ack_seq = 0;
    g_last_notify_log_ms = 0;
    g_notify_rx_count = 0;
    g_write_req_count = 0;
    g_write_cfm_ok_count = 0;
    g_write_cfm_fail_count = 0;
    g_write_submit_fail_count = 0;
    g_business_write_req_count = 0;
}

static int32_t milli_from_float(float value)
{
    float scaled = value * 1000.0f;
    return (int32_t)((scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f));
}

static bool seek_name_match(const sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL || seek_result->data == NULL) {
        return false;
    }

    uint8_t *data = seek_result->data;
    uint16_t len = seek_result->data_length;
    size_t name_len = strlen(SLE_LASER_SERVER_NAME);
    for (uint16_t i = 0; i < len;) {
        if ((i + 1U) >= len) {
            break;
        }
        uint8_t ad_type = data[i];
        uint8_t ad_len = data[i + 1U];
        if ((uint16_t)(i + 2U + ad_len) > len) {
            break;
        }
        if (ad_type == SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME && ad_len == name_len &&
            memcmp(&data[i + 2U], SLE_LASER_SERVER_NAME, name_len) == 0) {
            return true;
        }
        i += (uint16_t)ad_len + 2U;
    }
    return false;
}

static void config_seek_params(void)
{
    sle_seek_param_t param = {0};
    param.own_addr_type = 0;
    param.filter_duplicates = 0;
    param.seek_filter_policy = 0;
    param.seek_phys = 1;
    param.seek_type[0] = SLE_SEEK_ACTIVE;
    param.seek_interval[0] = 0x64;
    param.seek_window[0] = 0x64;
    sle_set_seek_param(&param);
}

static void start_seek_if_needed(void)
{
    if (g_seek_active || g_connect_pending || sle_laser_client_is_connected()) {
        return;
    }
    config_seek_params();
    (void)sle_start_seek();
}

static void start_service_discovery(void)
{
    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    (void)ssapc_find_structure(SLE_CLIENT_ID_DEFAULT, g_conn_id, &find_param);
}

static void request_exchange_info(void)
{
    ssap_exchange_info_t info = {0};
    info.mtu_size = 512;
    info.version = 1;
    errcode_t ret = ssapc_exchange_info_req(SLE_CLIENT_ID_DEFAULT, g_conn_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless tx] exchange info fail: 0x%x, fallback discovery\r\n", ret);
        start_service_discovery();
    }
}

static void sle_enable_cbk(errcode_t status)
{
    osal_printk("[wireless tx] sle enable: 0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        start_seek_if_needed();
    }
}

static void seek_result_cbk(sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL || g_connect_pending || sle_laser_client_is_connected()) {
        return;
    }
    if (memcmp(seek_result->addr.addr, g_target_addr, SLE_ADDR_LEN) != 0 && !seek_name_match(seek_result)) {
        return;
    }

    memcpy_s(&g_pending_addr, sizeof(g_pending_addr), &seek_result->addr, sizeof(seek_result->addr));
    g_connect_pending = true;
    osal_printk("[wireless tx] found LaserRX, stop seek then connect\r\n");
    (void)sle_stop_seek();
}

static void seek_enable_cbk(errcode_t status)
{
    g_seek_active = (status == ERRCODE_SLE_SUCCESS);
    osal_printk("[wireless tx] seek enable: 0x%x\r\n", status);
}

static void seek_disable_cbk(errcode_t status)
{
    g_seek_active = false;
    osal_printk("[wireless tx] seek disable: 0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS && g_connect_pending) {
        errcode_t ret = sle_connect_remote_device(&g_pending_addr);
        if (ret != ERRCODE_SLE_SUCCESS) {
            osal_printk("[wireless tx] connect submit fail: 0x%x\r\n", ret);
            g_connect_pending = false;
            start_seek_if_needed();
        }
    }
}

static void connect_state_changed_cbk(uint16_t conn_id,
                                      const sle_addr_t *addr,
                                      sle_acb_state_t conn_state,
                                      sle_pair_state_t pair_state,
                                      sle_disc_reason_t disc_reason)
{
    unused(addr);
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_id = conn_id;
        g_connect_pending = false;
        reset_link_runtime();
        osal_printk("[wireless tx] connected LaserRX conn_id=%u\r\n", conn_id);
        request_exchange_info();
        return;
    }

    if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_conn_id = SLE_CONN_INVALID;
        g_connect_pending = false;
        reset_link_runtime();
        osal_printk("[wireless tx] disconnected LaserRX\r\n");
        start_seek_if_needed();
    }
}

static void pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    osal_printk("[wireless tx] pair complete: 0x%x\r\n", status);
}

static void auth_complete_cbk(uint16_t conn_id,
                              const sle_addr_t *addr,
                              errcode_t status,
                              const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    osal_printk("[wireless tx] auth complete: 0x%x\r\n", status);
}

static void exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    if (status != ERRCODE_SLE_SUCCESS || param == NULL) {
        osal_printk("[wireless tx] exchange info cb fail: 0x%x\r\n", status);
    } else {
        osal_printk("[wireless tx] MTU: %u\r\n", param->mtu_size);
    }
    start_service_discovery();
}

static void find_structure_cbk(uint8_t client_id,
                               uint16_t conn_id,
                               ssapc_find_service_result_t *service,
                               errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || service == NULL || !uuid16_equals(&service->uuid, SLE_LASER_SERVICE_UUID)) {
        return;
    }

    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PROPERTY;
    find_param.start_hdl = service->start_hdl;
    find_param.end_hdl = service->end_hdl;
    osal_printk("[wireless tx] LaserRX service found: 0x%x-0x%x\r\n", service->start_hdl, service->end_hdl);
    (void)ssapc_find_structure(SLE_CLIENT_ID_DEFAULT, conn_id, &find_param);
}

static void find_property_cbk(uint8_t client_id,
                              uint16_t conn_id,
                              ssapc_find_property_result_t *property,
                              errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    if (status != ERRCODE_SLE_SUCCESS || property == NULL) {
        return;
    }
    if (uuid16_equals(&property->uuid, SLE_LASER_CMD_CHAR_UUID)) {
        g_cmd_handle = property->handle;
        osal_printk("[wireless tx] cmd handle: 0x%x\r\n", g_cmd_handle);
    } else if (uuid16_equals(&property->uuid, SLE_LASER_STATUS_CHAR_UUID)) {
        g_status_handle = property->handle;
        osal_printk("[wireless tx] status handle: 0x%x\r\n", g_status_handle);
    }
    g_handles_ready = (g_cmd_handle != 0U) && (g_status_handle != 0U);
}

static void find_structure_cmp_cbk(uint8_t client_id,
                                   uint16_t conn_id,
                                   ssapc_find_structure_result_t *result,
                                   errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    if (status == ERRCODE_SLE_SUCCESS && result != NULL && result->type == SSAP_FIND_TYPE_PROPERTY) {
        osal_printk("[wireless tx] discovery done handles=%u status_rx=%u\r\n", g_handles_ready ? 1U : 0U,
                    g_status_rx_seen ? 1U : 0U);
    }
}

static void write_cfm_cbk(uint8_t client_id, uint16_t conn_id, ssapc_write_result_t *write_result, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(write_result);
    {
        unsigned int irq_state = osal_irq_lock();
        if (g_pending_writes > 0U) {
            g_pending_writes--;
        }
        osal_irq_restore(irq_state);
    }
    if (status != ERRCODE_SLE_SUCCESS) {
        g_write_cfm_fail_count++;
        osal_printk("[wireless tx] write cfm fail: 0x%x\r\n", status);
        if (g_write_sem_ready) {
            osal_sem_up(&g_write_sem);
        }
        return;
    }
    g_write_cfm_ok_count++;
    if (g_write_sem_ready) {
        osal_sem_up(&g_write_sem);
    }
}

static void update_status_from_full(const status_full_pkt_t *full)
{
    uint16_t old_ack = g_last_ack_seq;
    g_feedback_seq++;
    g_remote_status = full->base.status;
    g_queue_free = full->base.queue_free;
    g_last_ack_seq = full->base.ack_seq;
    g_remote_x = (double)full->cur_x;
    g_remote_y = (double)full->cur_y;
    g_feedback_seq++;
    gcode_processor_update_feedback_pos(g_remote_x, g_remote_y);
    if (g_ack_sem_ready && old_ack != g_last_ack_seq) {
        if (g_last_ack_seq <= 16U || ((g_last_ack_seq % 64U) == 0U)) {
            osal_printk("[wireless tx] ack notify seq=%u free=%u full\r\n", g_last_ack_seq, g_queue_free);
        }
        osal_sem_up(&g_ack_sem);
    }
}

static void update_status_from_base(const status_pkt_t *base)
{
    uint16_t old_ack = g_last_ack_seq;
    g_feedback_seq++;
    g_remote_status = base->status;
    g_queue_free = base->queue_free;
    g_last_ack_seq = base->ack_seq;
    g_feedback_seq++;
    if (g_ack_sem_ready && old_ack != g_last_ack_seq) {
        if (g_last_ack_seq <= 16U || ((g_last_ack_seq % 64U) == 0U)) {
            osal_printk("[wireless tx] ack notify seq=%u free=%u\r\n", g_last_ack_seq, g_queue_free);
        }
        osal_sem_up(&g_ack_sem);
    }
}

static void notification_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    if (status != ERRCODE_SLE_SUCCESS || data == NULL || data->data == NULL || data->handle != g_status_handle) {
        return;
    }

    if (data->data_len >= sizeof(status_full_pkt_t)) {
        status_full_pkt_t full = {0};
        memcpy_s(&full, sizeof(full), data->data, sizeof(full));
        if (status_pkt_check_crc(&full.base)) {
            bool first = !g_status_rx_seen;
            g_status_rx_seen = true;
            update_status_from_full(&full);
            g_notify_rx_count++;
            if (first) {
                osal_printk("[wireless tx] LaserRX status link ready\r\n");
            }
            uint32_t now = (uint32_t)uapi_systick_get_ms();
            if ((g_last_notify_log_ms == 0) || ((uint32_t)(now - g_last_notify_log_ms) >= 10000U)) {
                g_last_notify_log_ms = now;
                osal_printk("[wireless tx] notify rx=%u ack=%u free=%u x_milli=%d y_milli=%d\r\n",
                            g_notify_rx_count, full.base.ack_seq, full.base.queue_free,
                            milli_from_float(full.cur_x), milli_from_float(full.cur_y));
            }
        }
    } else if (data->data_len >= sizeof(status_pkt_t)) {
        status_pkt_t base = {0};
        memcpy_s(&base, sizeof(base), data->data, sizeof(base));
        if (status_pkt_check_crc(&base)) {
            bool first = !g_status_rx_seen;
            g_status_rx_seen = true;
            update_status_from_base(&base);
            g_notify_rx_count++;
            if (first) {
                osal_printk("[wireless tx] LaserRX status link ready\r\n");
            }
            uint32_t now = (uint32_t)uapi_systick_get_ms();
            if ((g_last_notify_log_ms == 0) || ((uint32_t)(now - g_last_notify_log_ms) >= 10000U)) {
                g_last_notify_log_ms = now;
                osal_printk("[wireless tx] notify rx=%u ack=%u free=%u\r\n", g_notify_rx_count, base.ack_seq,
                            base.queue_free);
            }
        }
    }
}

static void indication_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    notification_cbk(client_id, conn_id, data, status);
}

errcode_t sle_laser_client_init(void)
{
    if (!g_ack_sem_ready) {
        if (osal_sem_init(&g_ack_sem, 0) != OSAL_SUCCESS) {
            osal_printk("[wireless tx] ack sem init failed\r\n");
            return ERRCODE_FAIL;
        }
        g_ack_sem_ready = true;
    }
    if (!g_write_sem_ready) {
        if (osal_sem_init(&g_write_sem, 0) != OSAL_SUCCESS) {
            osal_printk("[wireless tx] write sem init failed\r\n");
            return ERRCODE_FAIL;
        }
        g_write_sem_ready = true;
    }
    reset_link_runtime();
    g_conn_id = SLE_CONN_INVALID;
    g_seek_active = false;
    g_connect_pending = false;

    sle_announce_seek_callbacks_t seek_cbk = {0};
    seek_cbk.sle_enable_cb = sle_enable_cbk;
    seek_cbk.seek_enable_cb = seek_enable_cbk;
    seek_cbk.seek_disable_cb = seek_disable_cbk;
    seek_cbk.seek_result_cb = seek_result_cbk;
    sle_announce_seek_register_callbacks(&seek_cbk);

    sle_connection_callbacks_t conn_cbk = {0};
    conn_cbk.connect_state_changed_cb = connect_state_changed_cbk;
    conn_cbk.pair_complete_cb = pair_complete_cbk;
    conn_cbk.auth_complete_cb = auth_complete_cbk;
    sle_connection_register_callbacks(&conn_cbk);

    ssapc_callbacks_t ssapc_cbk = {0};
    ssapc_cbk.exchange_info_cb = exchange_info_cbk;
    ssapc_cbk.find_structure_cb = find_structure_cbk;
    ssapc_cbk.ssapc_find_property_cbk = find_property_cbk;
    ssapc_cbk.find_structure_cmp_cb = find_structure_cmp_cbk;
    ssapc_cbk.write_cfm_cb = write_cfm_cbk;
    ssapc_cbk.notification_cb = notification_cbk;
    ssapc_cbk.indication_cb = indication_cbk;
    ssapc_register_callbacks(&ssapc_cbk);

    errcode_t ret = enable_sle();
    osal_printk("[wireless tx] enable_sle called ret=0x%x\r\n", ret);
    return ret;
}

errcode_t sle_laser_client_send_cmd(const motion_cmd_t *cmd)
{
    if (cmd == NULL || !sle_laser_client_is_connected() || !g_handles_ready || g_cmd_handle == 0U) {
        return ERRCODE_SLE_FAIL;
    }

    bool need_cfm = cmd_requires_write_cfm(cmd);
    uint16_t pending_limit = (cmd->cmd == CMD_HEARTBEAT) ? SLE_TX_HEARTBEAT_MAX_PENDING : SLE_TX_BUSINESS_MAX_PENDING;
    if (cmd->cmd != CMD_HEARTBEAT && !sle_laser_client_can_send_heartbeat()) {
        return ERRCODE_SLE_FAIL;
    }
    if (need_cfm && g_pending_writes >= pending_limit) {
        return ERRCODE_SLE_BUSY;
    }

    ssapc_write_param_t param = {0};
    param.handle = g_cmd_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = sizeof(*cmd);
    param.data = (uint8_t *)cmd;
    errcode_t ret = need_cfm ? ssapc_write_req(SLE_CLIENT_ID_DEFAULT, g_conn_id, &param)
                             : ssapc_write_cmd(SLE_CLIENT_ID_DEFAULT, g_conn_id, &param);
    if (ret == ERRCODE_SLE_SUCCESS) {
        if (need_cfm) {
            unsigned int irq_state = osal_irq_lock();
            g_pending_writes++;
            osal_irq_restore(irq_state);
        } else {
            g_write_cfm_ok_count++;
        }
        g_write_req_count++;
        if (cmd->cmd != CMD_HEARTBEAT) {
            g_business_write_req_count++;
            g_last_business_write_ms = (uint32_t)uapi_systick_get_ms();
            if (g_business_write_req_count <= 16U || ((g_business_write_req_count % 64U) == 0U)) {
                osal_printk("[wireless tx] business write=%u seq=%u cmd=0x%x ack=%u pending=%u total_wr=%u cfm=%u\r\n",
                            g_business_write_req_count, cmd->seq, cmd->cmd, g_last_ack_seq,
                            g_pending_writes, g_write_req_count, need_cfm ? 1U : 0U);
            }
        }
        return ret;
    }

    g_write_submit_fail_count++;
    osal_printk("[wireless tx] write submit fail: cmd=0x%x seq=%u ret=0x%x pending=%u\r\n", cmd->cmd, cmd->seq,
                ret, g_pending_writes);
    return ret;
}

void sle_laser_client_note_business_activity(void)
{
    g_last_business_write_ms = (uint32_t)uapi_systick_get_ms();
}

bool sle_laser_client_is_connected(void)
{
    return g_conn_id != SLE_CONN_INVALID;
}

bool sle_laser_client_has_handles_ready(void)
{
    return g_handles_ready;
}

bool sle_laser_client_has_status_rx(void)
{
    return g_status_rx_seen;
}

bool sle_laser_client_can_send_heartbeat(void)
{
    return sle_laser_client_is_connected() && g_handles_ready;
}

bool sle_laser_client_is_ready(void)
{
    return sle_laser_client_can_send_heartbeat();
}

bool sle_laser_client_wait_write_idle(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;
    if (!g_write_sem_ready) {
        return g_pending_writes == 0U;
    }

    while (sle_laser_client_can_send_heartbeat()) {
        if (g_pending_writes == 0U) {
            return true;
        }
        if (waited_ms >= timeout_ms) {
            return false;
        }

        uint32_t wait_ms = timeout_ms - waited_ms;
        if (wait_ms > 50U) {
            wait_ms = 50U;
        }
        (void)osal_sem_down_timeout(&g_write_sem, wait_ms);
        waited_ms += wait_ms;
    }
    return false;
}

uint8_t sle_laser_client_get_remote_status(void)
{
    return g_remote_status;
}

void sle_laser_client_get_feedback_snapshot(uint8_t *status, double *x, double *y)
{
    uint32_t begin;
    uint32_t end;
    uint8_t local_status;
    double local_x;
    double local_y;
    do {
        begin = g_feedback_seq;
        if ((begin & 1U) != 0U) {
            continue;
        }
        local_status = g_remote_status;
        local_x = g_remote_x;
        local_y = g_remote_y;
        end = g_feedback_seq;
    } while (begin != end || ((end & 1U) != 0U));

    if (status != NULL) {
        *status = local_status;
    }
    if (x != NULL) {
        *x = local_x;
    }
    if (y != NULL) {
        *y = local_y;
    }
}

uint8_t sle_laser_client_get_queue_free(void)
{
    return g_queue_free;
}

uint16_t sle_laser_client_get_last_ack_seq(void)
{
    return g_last_ack_seq;
}

bool sle_laser_client_wait_ack(uint16_t seq, uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;
    if (!g_ack_sem_ready) {
        return seq_reached(g_last_ack_seq, seq);
    }

    while (sle_laser_client_can_send_heartbeat()) {
        if (seq_reached(g_last_ack_seq, seq)) {
            return true;
        }
        if (waited_ms >= timeout_ms) {
            return false;
        }

        uint32_t wait_ms = timeout_ms - waited_ms;
        if (wait_ms > 50U) {
            wait_ms = 50U;
        }
        (void)osal_sem_down_timeout(&g_ack_sem, wait_ms);
        waited_ms += wait_ms;
    }
    return false;
}

uint16_t sle_laser_client_get_cmd_handle(void)
{
    return g_cmd_handle;
}

uint16_t sle_laser_client_get_status_handle(void)
{
    return g_status_handle;
}

uint16_t sle_laser_client_get_pending_writes(void)
{
    return g_pending_writes;
}

uint32_t sle_laser_client_get_last_business_write_ms(void)
{
    return g_last_business_write_ms;
}

uint32_t sle_laser_client_get_write_req_count(void)
{
    return g_write_req_count;
}

uint32_t sle_laser_client_get_write_cfm_ok_count(void)
{
    return g_write_cfm_ok_count;
}

uint32_t sle_laser_client_get_write_cfm_fail_count(void)
{
    return g_write_cfm_fail_count;
}

uint32_t sle_laser_client_get_write_submit_fail_count(void)
{
    return g_write_submit_fail_count;
}

uint32_t sle_laser_client_get_notify_rx_count(void)
{
    return g_notify_rx_count;
}
