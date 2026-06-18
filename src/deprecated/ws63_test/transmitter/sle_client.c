/**
 * @file sle_client.c
 * @brief 发射板 SLE Client
 *        抽象“扫描/连接/发现/写入”的多节点公共层，
 *        由不同 peer 的状态回调完成业务解码。
 */
#include "sle_client.h"
#include "config.h"
#include "crc16.h"
#include "gcode_processor.h"
#include "systick.h"

#include "common_def.h"
#include "securec.h"
#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"
#include "soc_osal.h"
#include <string.h>

#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME
#define SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME 0x0B
#endif

#ifndef SLE_SEEK_ACTIVE
#define SLE_SEEK_ACTIVE 0x01
#endif

typedef enum {
    SLE_PEER_KIND_NONE = 0,
    SLE_PEER_KIND_LASER = 1,
    SLE_PEER_KIND_FOCUS = 2,
    SLE_PEER_KIND_SAFETY = 3,
} sle_peer_kind_t;

typedef struct sle_peer_runtime sle_peer_runtime_t;
typedef void (*sle_peer_feedback_reset_fn_t)(void);
typedef bool (*sle_peer_status_handler_fn_t)(sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len);

struct sle_peer_runtime {
    sle_peer_kind_t kind;
    const char *name;
    uint16_t service_uuid;
    uint16_t cmd_uuid;
    uint16_t status_uuid;
    uint8_t target_addr[SLE_ADDR_LEN];
    sle_peer_feedback_reset_fn_t reset_feedback;
    sle_peer_status_handler_fn_t handle_status;
    uint16_t conn_id;
    bool connected;
    bool handles_ready;
    bool status_rx_seen;
    bool exchange_started;
    uint16_t cmd_handle;
    uint16_t status_handle;
    volatile uint16_t pending_writes;
    volatile uint32_t last_business_write_ms;
    uint32_t write_req_count;
    uint32_t write_cfm_ok_count;
    uint32_t write_cfm_fail_count;
    uint32_t write_submit_fail_count;
};

#define SLE_PEER_INDEX_INVALID 0xFFU
#define SLE_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static void sle_reset_laser_feedback(void);
static void sle_reset_focus_feedback(void);
static void sle_reset_safety_feedback(void);
static bool sle_handle_laser_status(sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len);
static bool sle_handle_focus_status(sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len);
static bool sle_handle_safety_status(sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len);

static sle_peer_runtime_t g_peers[] = {
    {
        .kind = SLE_PEER_KIND_LASER,
        .name = SLE_LASER_SERVER_NAME,
        .service_uuid = SLE_LASER_SERVICE_UUID,
        .cmd_uuid = SLE_LASER_CMD_CHAR_UUID,
        .status_uuid = SLE_LASER_STATUS_CHAR_UUID,
        .target_addr = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01},
        .reset_feedback = sle_reset_laser_feedback,
        .handle_status = sle_handle_laser_status,
    },
    {
        .kind = SLE_PEER_KIND_FOCUS,
        .name = SLE_FOCUS_SERVER_NAME,
        .service_uuid = SLE_FOCUS_SERVICE_UUID,
        .cmd_uuid = SLE_FOCUS_CMD_CHAR_UUID,
        .status_uuid = SLE_FOCUS_STATUS_CHAR_UUID,
        .target_addr = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03},
        .reset_feedback = sle_reset_focus_feedback,
        .handle_status = sle_handle_focus_status,
    },
    {
        .kind = SLE_PEER_KIND_SAFETY,
        .name = SLE_SAFETY_SERVER_NAME,
        .service_uuid = SLE_SAFETY_SERVICE_UUID,
        .cmd_uuid = SLE_SAFETY_CMD_CHAR_UUID,
        .status_uuid = SLE_SAFETY_STATUS_CHAR_UUID,
        .target_addr = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x04},
        .reset_feedback = sle_reset_safety_feedback,
        .handle_status = sle_handle_safety_status,
    },
};
typedef char sle_peer_capacity_static_assert[(SLE_ARRAY_SIZE(g_peers) <= SLE_TX_MAX_PEERS) ? 1 : -1];

static bool g_seek_active = false;
static uint8_t g_connect_pending_index = SLE_PEER_INDEX_INVALID;
static sle_addr_t g_connect_pending_addr = {0};

static uint8_t g_remote_status = STATUS_IDLE;
static double g_remote_x = 0.0;
static double g_remote_y = 0.0;
static volatile uint32_t g_feedback_snapshot_seq = 0;
static uint8_t g_queue_free = 0;
static uint16_t g_last_ack_seq = 0;

static focus_node_status_t g_focus_remote_status = {
    .version = FOCUS_PROTOCOL_VERSION,
    .status = FOCUS_STATUS_ERROR,
    .error_code = FOCUS_ERR_Z_NOT_READY,
};
static volatile uint32_t g_focus_feedback_snapshot_seq = 0;

static safety_node_status_t g_safety_remote_status = {
    .version = SAFETY_PROTOCOL_VERSION,
    .status = SAFETY_STATUS_ERROR,
    .error_code = SAFETY_ERR_LED_IO,
};
static volatile uint32_t g_safety_feedback_snapshot_seq = 0;
static uint16_t g_safety_cmd_seq = 0;

static size_t sle_peer_count(void)
{
    return SLE_ARRAY_SIZE(g_peers);
}

static sle_peer_runtime_t *sle_get_peer_by_index(uint8_t index)
{
    if (index >= sle_peer_count()) {
        return NULL;
    }
    return &g_peers[index];
}

static sle_peer_runtime_t *sle_get_pending_peer(void)
{
    return sle_get_peer_by_index(g_connect_pending_index);
}

static void sle_clear_connect_pending(void)
{
    g_connect_pending_index = SLE_PEER_INDEX_INVALID;
    memset(&g_connect_pending_addr, 0, sizeof(g_connect_pending_addr));
}

static sle_peer_runtime_t *sle_get_peer(sle_peer_kind_t kind)
{
    size_t i;

    for (i = 0; i < sle_peer_count(); i++) {
        if (g_peers[i].kind == kind) {
            return &g_peers[i];
        }
    }
    return NULL;
}

static sle_peer_runtime_t *sle_get_peer_by_conn(uint16_t conn_id)
{
    size_t i;

    for (i = 0; i < sle_peer_count(); i++) {
        if (g_peers[i].connected && (g_peers[i].conn_id == conn_id)) {
            return &g_peers[i];
        }
    }
    return NULL;
}

static sle_peer_runtime_t *sle_get_peer_by_addr(const sle_addr_t *addr)
{
    size_t i;

    if (addr == NULL) {
        return NULL;
    }

    for (i = 0; i < sle_peer_count(); i++) {
        if (memcmp(addr->addr, g_peers[i].target_addr, SLE_ADDR_LEN) == 0) {
            return &g_peers[i];
        }
    }
    return NULL;
}

static void sle_reset_laser_feedback(void)
{
    g_remote_status = STATUS_IDLE;
    g_remote_x = 0.0;
    g_remote_y = 0.0;
    g_feedback_snapshot_seq = 0;
    g_queue_free = 0;
    g_last_ack_seq = 0;
}

static void sle_reset_focus_feedback(void)
{
    memset(&g_focus_remote_status, 0, sizeof(g_focus_remote_status));
    g_focus_remote_status.version = FOCUS_PROTOCOL_VERSION;
    g_focus_remote_status.status = FOCUS_STATUS_ERROR;
    g_focus_remote_status.error_code = FOCUS_ERR_Z_NOT_READY;
    g_focus_feedback_snapshot_seq = 0;
}

static void sle_reset_safety_feedback(void)
{
    memset(&g_safety_remote_status, 0, sizeof(g_safety_remote_status));
    g_safety_remote_status.version = SAFETY_PROTOCOL_VERSION;
    g_safety_remote_status.status = SAFETY_STATUS_ERROR;
    g_safety_remote_status.error_code = SAFETY_ERR_LED_IO;
    g_safety_feedback_snapshot_seq = 0;
}

static void sle_reset_peer_runtime(sle_peer_runtime_t *peer)
{
    if (peer == NULL) {
        return;
    }

    peer->handles_ready = false;
    peer->status_rx_seen = false;
    peer->exchange_started = false;
    peer->cmd_handle = 0;
    peer->status_handle = 0;
    peer->pending_writes = 0;
    peer->last_business_write_ms = 0;
    peer->write_req_count = 0;
    peer->write_cfm_ok_count = 0;
    peer->write_cfm_fail_count = 0;
    peer->write_submit_fail_count = 0;

    if (peer->reset_feedback != NULL) {
        peer->reset_feedback();
    }
}

static bool sle_all_peers_connected(void)
{
    size_t i;

    for (i = 0; i < sle_peer_count(); i++) {
        if (!g_peers[i].connected) {
            return false;
        }
    }
    return true;
}

static void sle_start_service_discovery(sle_peer_runtime_t *peer)
{
    ssapc_find_structure_param_t find_param = {0};

    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    ssapc_find_structure(0, peer->conn_id, &find_param);
}

static void sle_request_exchange_info(sle_peer_runtime_t *peer)
{
    ssap_exchange_info_t info = {0};
    errcode_t ret;

    if ((peer == NULL) || peer->exchange_started) {
        return;
    }

    peer->exchange_started = true;
    info.mtu_size = 512;
    info.version = 1;
    ret = ssapc_exchange_info_req(0, peer->conn_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser tx] exchange info req fail for %s: 0x%x, fallback discovery\r\n", peer->name, ret);
        sle_start_service_discovery(peer);
    }
}

static bool uuid16_equals(const sle_uuid_t *uuid, uint16_t expect)
{
    if ((uuid == NULL) || (uuid->len != 2)) {
        return false;
    }
    return (uuid->uuid[14] == (uint8_t)(expect & 0xFF)) && (uuid->uuid[15] == (uint8_t)((expect >> 8) & 0xFF));
}

static void sle_config_seek_params(void)
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

static void sle_start_seek_if_needed(void)
{
    if (g_seek_active || (g_connect_pending_index != SLE_PEER_INDEX_INVALID) || sle_all_peers_connected()) {
        return;
    }

    sle_config_seek_params();
    sle_start_seek();
}

static bool sle_seek_name_match(const sle_seek_result_info_t *seek_result, const char *name)
{
    uint8_t *data;
    uint16_t len;
    uint16_t i;
    size_t name_len;

    if ((seek_result == NULL) || (seek_result->data == NULL) || (name == NULL)) {
        return false;
    }

    data = seek_result->data;
    len = seek_result->data_length;
    name_len = strlen(name);
    for (i = 0; i < len;) {
        uint8_t ad_type;
        uint8_t ad_len;

        if ((i + 1U) >= len) {
            break;
        }
        ad_type = data[i];
        ad_len = data[i + 1U];
        if ((uint16_t)(i + 2U + ad_len) > len) {
            break;
        }

        if ((ad_type == SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME) && (ad_len == name_len) &&
            (memcmp(&data[i + 2U], name, name_len) == 0)) {
            return true;
        }
        i += (uint16_t)ad_len + 2U;
    }

    return false;
}

static sle_peer_runtime_t *sle_match_peer_from_seek(const sle_seek_result_info_t *seek_result)
{
    size_t i;

    for (i = 0; i < sle_peer_count(); i++) {
        sle_peer_runtime_t *peer = &g_peers[i];
        if (peer->connected || (g_connect_pending_index == i)) {
            continue;
        }
        if (memcmp(seek_result->addr.addr, peer->target_addr, SLE_ADDR_LEN) == 0) {
            return peer;
        }
        if (sle_seek_name_match(seek_result, peer->name)) {
            return peer;
        }
    }
    return NULL;
}

static errcode_t sle_send_packet(
    sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len, bool track_business, uint16_t pending_limit)
{
    ssapc_write_param_t param = {0};
    errcode_t ret;

    if ((peer == NULL) || !peer->connected || !peer->handles_ready || (peer->cmd_handle == 0U) || (data == NULL) ||
        (data_len == 0U)) {
        return ERRCODE_SLE_FAIL;
    }

    if (peer->pending_writes >= pending_limit) {
        return ERRCODE_SLE_BUSY;
    }

    param.handle = peer->cmd_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = data_len;
    param.data = (uint8_t *)data;
    ret = ssapc_write_req(0, peer->conn_id, &param);
    if (ret == ERRCODE_SLE_SUCCESS) {
        peer->pending_writes++;
        if (track_business) {
            peer->last_business_write_ms = (uint32_t)uapi_systick_get_ms();
        }
        peer->write_req_count++;
        return ret;
    }

    peer->write_submit_fail_count++;
    if ((peer->write_submit_fail_count % 20U) == 1U) {
        osal_printk("[laser tx] write submit fail for %s: 0x%x (pending=%u submit_fail=%u)\r\n", peer->name, ret,
                    peer->pending_writes, peer->write_submit_fail_count);
    }
    return ret;
}

static void sle_update_laser_feedback_from_full(const status_full_pkt_t *full)
{
    g_feedback_snapshot_seq++;
    g_remote_status = full->base.status;
    g_last_ack_seq = full->base.ack_seq;
    g_queue_free = full->base.queue_free;
    g_remote_x = (double)full->cur_x;
    g_remote_y = (double)full->cur_y;
    g_feedback_snapshot_seq++;
    gcode_processor_update_feedback_pos(full->cur_x, full->cur_y);
}

static void sle_update_laser_feedback_from_base(const status_pkt_t *base)
{
    g_feedback_snapshot_seq++;
    g_remote_status = base->status;
    g_last_ack_seq = base->ack_seq;
    g_queue_free = base->queue_free;
    g_feedback_snapshot_seq++;
}

static void sle_update_focus_feedback(const focus_node_status_t *status)
{
    g_focus_feedback_snapshot_seq++;
    g_focus_remote_status = *status;
    g_focus_feedback_snapshot_seq++;
}

static void sle_update_safety_feedback(const safety_node_status_t *status)
{
    g_safety_feedback_snapshot_seq++;
    g_safety_remote_status = *status;
    g_safety_feedback_snapshot_seq++;
}

static bool sle_handle_laser_status(sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len)
{
    if ((peer == NULL) || (data == NULL)) {
        return false;
    }

    if (data_len >= sizeof(status_full_pkt_t)) {
        status_full_pkt_t full = {0};
        bool first_status;

        memcpy(&full, data, sizeof(full));
        if (!status_pkt_check_crc(&full.base)) {
            return false;
        }

        first_status = !peer->status_rx_seen;
        peer->status_rx_seen = true;
        sle_update_laser_feedback_from_full(&full);
        if (first_status) {
            osal_printk("[laser tx] %s status link ready\r\n", peer->name);
        }
        return true;
    }

    if (data_len >= sizeof(status_pkt_t)) {
        status_pkt_t base = {0};
        bool first_status;

        memcpy(&base, data, sizeof(base));
        if (!status_pkt_check_crc(&base)) {
            return false;
        }

        first_status = !peer->status_rx_seen;
        peer->status_rx_seen = true;
        sle_update_laser_feedback_from_base(&base);
        if (first_status) {
            osal_printk("[laser tx] %s status link ready\r\n", peer->name);
        }
        return true;
    }

    return false;
}

static bool sle_handle_focus_status(sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len)
{
    focus_node_status_t focus = {0};
    bool first_status;

    if ((peer == NULL) || (data == NULL) || (data_len < sizeof(focus_node_status_t))) {
        return false;
    }

    memcpy(&focus, data, sizeof(focus));
    if (!focus_status_check_crc(&focus)) {
        return false;
    }

    first_status = !peer->status_rx_seen;
    peer->status_rx_seen = true;
    sle_update_focus_feedback(&focus);
    if (first_status) {
        osal_printk("[laser tx] %s status link ready\r\n", peer->name);
    }
    return true;
}

static bool sle_handle_safety_status(sle_peer_runtime_t *peer, const uint8_t *data, uint16_t data_len)
{
    safety_node_status_t safety = {0};
    bool first_status;

    if ((peer == NULL) || (data == NULL) || (data_len < sizeof(safety_node_status_t))) {
        return false;
    }

    memcpy(&safety, data, sizeof(safety));
    if (!safety_status_check_crc(&safety)) {
        return false;
    }

    first_status = !peer->status_rx_seen;
    peer->status_rx_seen = true;
    sle_update_safety_feedback(&safety);
    if (first_status) {
        osal_printk("[laser tx] %s status link ready\r\n", peer->name);
    }
    return true;
}

static void sle_sample_sle_enable_cbk(errcode_t status)
{
    osal_printk("[laser tx] sle enable: 0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        sle_start_seek_if_needed();
    }
}

static void sle_sample_seek_result_cbk(sle_seek_result_info_t *seek_result)
{
    sle_peer_runtime_t *peer;
    size_t peer_index;

    if ((seek_result == NULL) || (g_connect_pending_index != SLE_PEER_INDEX_INVALID) || sle_all_peers_connected()) {
        return;
    }

    peer = sle_match_peer_from_seek(seek_result);
    if (peer == NULL) {
        return;
    }

    peer_index = (size_t)(peer - g_peers);
    memcpy_s(&g_connect_pending_addr, sizeof(g_connect_pending_addr), &seek_result->addr, sizeof(seek_result->addr));
    g_connect_pending_index = (uint8_t)peer_index;
    osal_printk("[laser tx] found %s, stop seek then connect\r\n", peer->name);
    sle_stop_seek();
}

static void sle_sample_seek_enable_cbk(errcode_t status)
{
    g_seek_active = (status == ERRCODE_SLE_SUCCESS);
    osal_printk("[laser tx] seek enable: 0x%x\r\n", status);
}

static void sle_sample_seek_disable_cbk(errcode_t status)
{
    errcode_t ret;

    g_seek_active = false;
    osal_printk("[laser tx] seek disable: 0x%x\r\n", status);
    if ((status == ERRCODE_SLE_SUCCESS) && (g_connect_pending_index != SLE_PEER_INDEX_INVALID)) {
        ret = sle_connect_remote_device(&g_connect_pending_addr);
        if (ret != ERRCODE_SLE_SUCCESS) {
            osal_printk("[laser tx] connect request submit fail: 0x%x\r\n", ret);
            sle_clear_connect_pending();
            sle_start_seek_if_needed();
        }
    }
}

static void sle_connect_state_changed_cbk(
    uint16_t conn_id, const sle_addr_t *addr, sle_acb_state_t conn_state, sle_pair_state_t pair_state,
    sle_disc_reason_t disc_reason)
{
    sle_peer_runtime_t *peer = NULL;

    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        if (g_connect_pending_index != SLE_PEER_INDEX_INVALID) {
            peer = sle_get_pending_peer();
        }
        if (peer == NULL) {
            peer = sle_get_peer_by_addr(addr);
        }
        if (peer == NULL) {
            osal_printk("[laser tx] connected on unknown peer, conn_id=%u\r\n", conn_id);
            sle_clear_connect_pending();
            sle_start_seek_if_needed();
            return;
        }

        peer->conn_id = conn_id;
        peer->connected = true;
        sle_reset_peer_runtime(peer);
        osal_printk("[laser tx] connected to %s, conn_id=%u\r\n", peer->name, conn_id);

        if (peer == sle_get_pending_peer()) {
            sle_clear_connect_pending();
        }

        sle_request_exchange_info(peer);
        sle_start_seek_if_needed();
        return;
    }

    if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        peer = sle_get_peer_by_conn(conn_id);
        if (peer == NULL) {
            peer = sle_get_peer_by_addr(addr);
        }
        if (peer != NULL) {
            osal_printk("[laser tx] disconnected from %s\r\n", peer->name);
            peer->connected = false;
            peer->conn_id = 0;
            sle_reset_peer_runtime(peer);
        } else {
            osal_printk("[laser tx] disconnected unknown peer, conn_id=%u\r\n", conn_id);
        }
        if ((g_connect_pending_index != SLE_PEER_INDEX_INVALID) && (addr != NULL) &&
            (memcmp(addr->addr, g_connect_pending_addr.addr, SLE_ADDR_LEN) == 0)) {
            sle_clear_connect_pending();
        }
        sle_start_seek_if_needed();
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    osal_printk("[laser tx] pair complete: 0x%x\r\n", status);
}

static void sle_auth_complete_cbk(
    uint16_t conn_id, const sle_addr_t *addr, errcode_t status, const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    osal_printk("[laser tx] auth complete: 0x%x\r\n", status);
}

static void sle_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status)
{
    sle_peer_runtime_t *peer = sle_get_peer_by_conn(conn_id);

    unused(client_id);

    if (peer == NULL) {
        return;
    }
    if ((status != ERRCODE_SLE_SUCCESS) || (param == NULL)) {
        osal_printk("[laser tx] exchange info failed for %s: 0x%x\r\n", peer->name, status);
        sle_start_service_discovery(peer);
        return;
    }

    osal_printk("[laser tx] %s MTU: %d\r\n", peer->name, param->mtu_size);
    sle_start_service_discovery(peer);
}

static void sle_find_structure_cbk(
    uint8_t client_id, uint16_t conn_id, ssapc_find_service_result_t *service, errcode_t status)
{
    sle_peer_runtime_t *peer = sle_get_peer_by_conn(conn_id);
    ssapc_find_structure_param_t find_param = {0};

    unused(client_id);

    if ((peer == NULL) || (status != ERRCODE_SLE_SUCCESS) || (service == NULL)) {
        return;
    }

    if (!uuid16_equals(&service->uuid, peer->service_uuid)) {
        return;
    }

    osal_printk("[laser tx] %s service found: start=0x%x end=0x%x\r\n", peer->name, service->start_hdl,
                service->end_hdl);
    find_param.type = SSAP_FIND_TYPE_PROPERTY;
    find_param.start_hdl = service->start_hdl;
    find_param.end_hdl = service->end_hdl;
    ssapc_find_structure(0, conn_id, &find_param);
}

static void sle_find_property_cbk(
    uint8_t client_id, uint16_t conn_id, ssapc_find_property_result_t *property, errcode_t status)
{
    sle_peer_runtime_t *peer = sle_get_peer_by_conn(conn_id);

    unused(client_id);

    if ((peer == NULL) || (status != ERRCODE_SLE_SUCCESS) || (property == NULL)) {
        return;
    }

    if (uuid16_equals(&property->uuid, peer->cmd_uuid)) {
        peer->cmd_handle = property->handle;
        osal_printk("[laser tx] %s cmd handle: 0x%x\r\n", peer->name, peer->cmd_handle);
    } else if (uuid16_equals(&property->uuid, peer->status_uuid)) {
        peer->status_handle = property->handle;
        osal_printk("[laser tx] %s status handle: 0x%x\r\n", peer->name, peer->status_handle);
    }

    peer->handles_ready = (peer->cmd_handle != 0U) && (peer->status_handle != 0U);
}

static void sle_find_structure_cmp_cbk(
    uint8_t client_id, uint16_t conn_id, ssapc_find_structure_result_t *result, errcode_t status)
{
    sle_peer_runtime_t *peer = sle_get_peer_by_conn(conn_id);

    unused(client_id);

    if ((peer == NULL) || (status != ERRCODE_SLE_SUCCESS) || (result == NULL)) {
        return;
    }

    if (result->type == SSAP_FIND_TYPE_PROPERTY) {
        osal_printk("[laser tx] %s discovery done, handles_ready=%d status_rx=%d\r\n", peer->name,
                    peer->handles_ready ? 1 : 0, peer->status_rx_seen ? 1 : 0);
    }
}

static void sle_write_cfm_cbk(
    uint8_t client_id, uint16_t conn_id, ssapc_write_result_t *write_result, errcode_t status)
{
    sle_peer_runtime_t *peer = sle_get_peer_by_conn(conn_id);

    unused(client_id);
    unused(write_result);

    if (peer == NULL) {
        return;
    }

    if (peer->pending_writes > 0U) {
        peer->pending_writes--;
    }

    if (status == ERRCODE_SLE_SUCCESS) {
        peer->write_cfm_ok_count++;
        return;
    }

    peer->write_cfm_fail_count++;
    if ((peer->write_cfm_fail_count % 20U) == 1U) {
        osal_printk("[laser tx] write cfm fail for %s: 0x%x (ok=%u fail=%u)\r\n", peer->name, status,
                    peer->write_cfm_ok_count, peer->write_cfm_fail_count);
    }
}

static void sle_notification_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    sle_peer_runtime_t *peer = sle_get_peer_by_conn(conn_id);

    unused(client_id);

    if ((peer == NULL) || (status != ERRCODE_SLE_SUCCESS) || (data == NULL) || (data->data == NULL)) {
        return;
    }
    if ((peer->status_handle == 0U) || (data->handle != peer->status_handle)) {
        return;
    }
    if (peer->handle_status != NULL) {
        (void)peer->handle_status(peer, data->data, data->data_len);
    }
}

static void sle_indication_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

errcode_t sle_laser_client_init(void)
{
    sle_announce_seek_callbacks_t seek_cbk = {0};
    sle_connection_callbacks_t conn_cbk = {0};
    ssapc_callbacks_t ssapc_cbk = {0};
    size_t i;

    for (i = 0; i < sle_peer_count(); i++) {
        g_peers[i].connected = false;
        g_peers[i].conn_id = 0;
        sle_reset_peer_runtime(&g_peers[i]);
    }
    g_seek_active = false;
    sle_clear_connect_pending();
    g_safety_cmd_seq = 0;

    seek_cbk.sle_enable_cb = sle_sample_sle_enable_cbk;
    seek_cbk.seek_enable_cb = sle_sample_seek_enable_cbk;
    seek_cbk.seek_disable_cb = sle_sample_seek_disable_cbk;
    seek_cbk.seek_result_cb = sle_sample_seek_result_cbk;
    sle_announce_seek_register_callbacks(&seek_cbk);

    conn_cbk.connect_state_changed_cb = sle_connect_state_changed_cbk;
    conn_cbk.pair_complete_cb = sle_pair_complete_cbk;
    conn_cbk.auth_complete_cb = sle_auth_complete_cbk;
    sle_connection_register_callbacks(&conn_cbk);

    ssapc_cbk.exchange_info_cb = sle_exchange_info_cbk;
    ssapc_cbk.find_structure_cb = sle_find_structure_cbk;
    ssapc_cbk.ssapc_find_property_cbk = sle_find_property_cbk;
    ssapc_cbk.find_structure_cmp_cb = sle_find_structure_cmp_cbk;
    ssapc_cbk.write_cfm_cb = sle_write_cfm_cbk;
    ssapc_cbk.notification_cb = sle_notification_cbk;
    ssapc_cbk.indication_cb = sle_indication_cbk;
    ssapc_register_callbacks(&ssapc_cbk);

    enable_sle();
    osal_printk("[laser tx] SLE enable called, configured peers=%u, framework capacity=%u\r\n",
                (unsigned int)sle_peer_count(), (unsigned int)SLE_TX_MAX_PEERS);
    return ERRCODE_SLE_SUCCESS;
}

uint8_t sle_client_get_connected_peer_count(void)
{
    size_t i;
    uint8_t count = 0;

    for (i = 0; i < sle_peer_count(); i++) {
        if (g_peers[i].connected) {
            count++;
        }
    }
    return count;
}

uint8_t sle_client_get_configured_peer_count(void)
{
    return (uint8_t)sle_peer_count();
}

errcode_t sle_laser_client_send_cmd(const motion_cmd_t *cmd)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    uint16_t pending_limit = SLE_TX_BUSINESS_MAX_PENDING;

    if ((cmd == NULL) || (peer == NULL)) {
        return ERRCODE_SLE_FAIL;
    }
    if (cmd->cmd == CMD_HEARTBEAT) {
        if (!sle_laser_client_can_send_heartbeat()) {
            return ERRCODE_SLE_FAIL;
        }
        pending_limit = SLE_TX_HEARTBEAT_MAX_PENDING;
    } else if (!sle_laser_client_is_ready()) {
        return ERRCODE_SLE_FAIL;
    }

    return sle_send_packet(peer, (const uint8_t *)cmd, sizeof(*cmd), (cmd->cmd != CMD_HEARTBEAT), pending_limit);
}

bool sle_laser_client_is_connected(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) && peer->connected;
}

bool sle_laser_client_can_send_heartbeat(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) && peer->connected && peer->handles_ready;
}

bool sle_laser_client_is_ready(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) && peer->connected && peer->handles_ready && peer->status_rx_seen;
}

bool sle_laser_client_has_handles_ready(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) && peer->handles_ready;
}

bool sle_laser_client_has_status_rx(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) && peer->status_rx_seen;
}

uint8_t sle_laser_client_get_remote_status(void)
{
    return g_remote_status;
}

void sle_laser_client_get_feedback_snapshot(uint8_t *status, double *x, double *y)
{
    uint32_t seq_begin;
    uint32_t seq_end;
    uint8_t local_status;
    double local_x;
    double local_y;

    do {
        seq_begin = g_feedback_snapshot_seq;
        if ((seq_begin & 1U) != 0U) {
            continue;
        }
        local_status = g_remote_status;
        local_x = g_remote_x;
        local_y = g_remote_y;
        seq_end = g_feedback_snapshot_seq;
    } while ((seq_begin != seq_end) || ((seq_end & 1U) != 0U));

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

uint16_t sle_laser_client_get_cmd_handle(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->cmd_handle : 0;
}

uint16_t sle_laser_client_get_status_handle(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->status_handle : 0;
}

uint16_t sle_laser_client_get_pending_writes(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->pending_writes : 0;
}

uint32_t sle_laser_client_get_last_business_write_ms(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->last_business_write_ms : 0;
}

uint32_t sle_laser_client_get_write_req_count(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->write_req_count : 0;
}

uint32_t sle_laser_client_get_write_cfm_ok_count(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->write_cfm_ok_count : 0;
}

uint32_t sle_laser_client_get_write_cfm_fail_count(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->write_cfm_fail_count : 0;
}

uint32_t sle_laser_client_get_write_submit_fail_count(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_LASER);
    return (peer != NULL) ? peer->write_submit_fail_count : 0;
}

errcode_t sle_focus_client_send_cmd(const focus_node_cmd_t *cmd)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);

    if ((cmd == NULL) || (peer == NULL)) {
        return ERRCODE_SLE_FAIL;
    }
    return sle_send_packet(peer, (const uint8_t *)cmd, sizeof(*cmd), true, SLE_TX_BUSINESS_MAX_PENDING);
}

bool sle_focus_client_is_connected(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);
    return (peer != NULL) && peer->connected;
}

bool sle_focus_client_has_handles_ready(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);
    return (peer != NULL) && peer->handles_ready;
}

bool sle_focus_client_has_status_rx(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);
    return (peer != NULL) && peer->status_rx_seen;
}

bool sle_focus_client_is_ready(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);
    return (peer != NULL) && peer->connected && peer->handles_ready && peer->status_rx_seen;
}

void sle_focus_client_get_status_snapshot(focus_node_status_t *status)
{
    uint32_t seq_begin;
    uint32_t seq_end;
    focus_node_status_t local_status;

    if (status == NULL) {
        return;
    }

    do {
        seq_begin = g_focus_feedback_snapshot_seq;
        if ((seq_begin & 1U) != 0U) {
            continue;
        }
        local_status = g_focus_remote_status;
        seq_end = g_focus_feedback_snapshot_seq;
    } while ((seq_begin != seq_end) || ((seq_end & 1U) != 0U));

    *status = local_status;
}

uint16_t sle_focus_client_get_cmd_handle(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);
    return (peer != NULL) ? peer->cmd_handle : 0;
}

uint16_t sle_focus_client_get_status_handle(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);
    return (peer != NULL) ? peer->status_handle : 0;
}

uint16_t sle_focus_client_get_pending_writes(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_FOCUS);
    return (peer != NULL) ? peer->pending_writes : 0;
}

errcode_t sle_safety_client_send_cmd(const safety_node_cmd_t *cmd)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);

    if ((cmd == NULL) || (peer == NULL)) {
        return ERRCODE_SLE_FAIL;
    }
    return sle_send_packet(peer, (const uint8_t *)cmd, sizeof(*cmd), true, SLE_TX_BUSINESS_MAX_PENDING);
}

errcode_t sle_safety_client_led_on(void)
{
    safety_node_cmd_t cmd = {0};

    cmd.version = SAFETY_PROTOCOL_VERSION;
    cmd.cmd = SAFETY_CMD_LED_ON;
    cmd.seq = ++g_safety_cmd_seq;
    safety_cmd_set_crc(&cmd);
    return sle_safety_client_send_cmd(&cmd);
}

errcode_t sle_safety_client_led_off(void)
{
    safety_node_cmd_t cmd = {0};

    cmd.version = SAFETY_PROTOCOL_VERSION;
    cmd.cmd = SAFETY_CMD_LED_OFF;
    cmd.seq = ++g_safety_cmd_seq;
    safety_cmd_set_crc(&cmd);
    return sle_safety_client_send_cmd(&cmd);
}

bool sle_safety_client_is_connected(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);
    return (peer != NULL) && peer->connected;
}

bool sle_safety_client_has_handles_ready(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);
    return (peer != NULL) && peer->handles_ready;
}

bool sle_safety_client_has_status_rx(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);
    return (peer != NULL) && peer->status_rx_seen;
}

bool sle_safety_client_is_ready(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);
    return (peer != NULL) && peer->connected && peer->handles_ready && peer->status_rx_seen;
}

void sle_safety_client_get_status_snapshot(safety_node_status_t *status)
{
    uint32_t seq_begin;
    uint32_t seq_end;
    safety_node_status_t local_status;

    if (status == NULL) {
        return;
    }

    do {
        seq_begin = g_safety_feedback_snapshot_seq;
        if ((seq_begin & 1U) != 0U) {
            continue;
        }
        local_status = g_safety_remote_status;
        seq_end = g_safety_feedback_snapshot_seq;
    } while ((seq_begin != seq_end) || ((seq_end & 1U) != 0U));

    *status = local_status;
}

uint16_t sle_safety_client_get_cmd_handle(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);
    return (peer != NULL) ? peer->cmd_handle : 0;
}

uint16_t sle_safety_client_get_status_handle(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);
    return (peer != NULL) ? peer->status_handle : 0;
}

uint16_t sle_safety_client_get_pending_writes(void)
{
    sle_peer_runtime_t *peer = sle_get_peer(SLE_PEER_KIND_SAFETY);
    return (peer != NULL) ? peer->pending_writes : 0;
}
