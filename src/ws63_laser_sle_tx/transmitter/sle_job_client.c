/**
 * @file sle_job_client.c
 * @brief SLE connection for bidirectional raw byte job.
 */
#include "sle_job_client.h"
#include "common_def.h"
#include "config.h"
#include "errcode.h"
#include "protocol.h"
#include "securec.h"
#include "soc_osal.h"
#include "systick.h"
#include <string.h>

/* SLE headers */
#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"

#define SLE_CLIENT_ID 0
#define SLE_CONN_INVALID 0xFFFF
#define UUID_LEN_2 2
#define CONNECT_RETRY_INTERVAL_MS 1000U

#define SLE_LINK_ROLE_NONE 0U
#define SLE_LINK_ROLE_RX 1U
#define SLE_LINK_ROLE_PANEL 2U
#define JOB_SLE_WRITE_CALL_SLOW_MS 20U

#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME
#define SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME 0x0B
#endif

#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS
#define SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS 0x05
#endif

#ifndef SLE_SEEK_ACTIVE
#define SLE_SEEK_ACTIVE 0x01
#endif

/* State */
static uint16_t g_conn_id = SLE_CONN_INVALID;
static uint16_t g_panel_conn_id = SLE_CONN_INVALID;
static bool g_panel_link_allowed = true;
static bool g_background_seek_allowed = true;
static bool g_sle_enabled = false;
static bool g_seek_active = false;
static bool g_connecting = false;
static uint8_t g_pending_role = SLE_LINK_ROLE_NONE;
static sle_addr_t g_pending_addr = {0};
static sle_addr_t g_panel_addr = {0};
static bool g_panel_addr_valid = false;
static bool g_handles_ready = false;
static bool g_panel_handles_ready = false;
static uint16_t g_data_handle = 0;
static uint16_t g_resp_handle = 0;
static uint16_t g_panel_status_handle = 0;
static sle_job_response_cb_t g_response_cb = NULL;
static uint32_t g_last_connect_ms = 0;
static osal_semaphore g_write_cfm_sem;
static volatile errcode_t g_last_write_cfm_status = ERRCODE_SLE_FAIL;
static volatile bool g_write_cfm_sem_ready = false;

/* Compatible with current ws63_sle_laser and ws63_test receiver defaults. */
static uint8_t g_receiver_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x01};
static uint8_t g_test_receiver_mac[SLE_ADDR_LEN] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

static bool uuid16_equals(const sle_uuid_t *uuid, uint16_t expect)
{
    return (uuid != NULL) && (uuid->len == UUID_LEN_2) &&
           (uuid->uuid[14] == (uint8_t)(expect & 0xFF)) &&
           (uuid->uuid[15] == (uint8_t)((expect >> 8) & 0xFF));
}

static uint16_t sle_job_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t sle_job_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void trace_response_notification(uint16_t conn_id, uint16_t handle,
    const ssapc_handle_value_t *data)
{
    if (data == NULL || data->data == NULL || data->data_len < SLE_JOB_PACKET_HEADER_LEN) {
        return;
    }

    const uint8_t *bytes = data->data;
    if (sle_job_le16(&bytes[0]) != SLE_JOB_PACKET_MAGIC) {
        return;
    }

    uint8_t pkt_type = bytes[2];
    uint16_t seq = sle_job_le16(&bytes[4]);
    uint16_t payload_len = sle_job_le16(&bytes[6]);
    if ((uint16_t)(SLE_JOB_PACKET_HEADER_LEN + payload_len) > data->data_len) {
        return;
    }

    if ((pkt_type == PKT_ACK || pkt_type == PKT_NACK) &&
        payload_len == sizeof(ack_payload_t)) {
        const uint8_t *payload = &bytes[SLE_JOB_PACKET_HEADER_LEN];
        uint8_t ack_type = payload[0];
        uint8_t status = payload[1];
        uint16_t ack_seq = sle_job_le16(&payload[2]);
        uint32_t offset = sle_job_le32(&payload[8]);
        uint32_t credit = sle_job_le32(&payload[12]);
        if (ack_type != PKT_JOB_DATA || status != JOB_STATUS_OK) {
            osal_printk("[TX_NOTIFY] t=%u conn=%u handle=0x%x len=%u pkt=0x%02x seq=%u "
                        "ack_type=0x%02x ack_seq=%u st=%u off=%u credit=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(),
                        (unsigned int)conn_id,
                        (unsigned int)handle,
                        (unsigned int)data->data_len,
                        (unsigned int)pkt_type,
                        (unsigned int)seq,
                        (unsigned int)ack_type,
                        (unsigned int)ack_seq,
                        (unsigned int)status,
                        (unsigned int)offset,
                        (unsigned int)credit);
        }
    }
}

static bool adv_name_match(const sle_seek_result_info_t *seek_result, const char *name)
{
    if (seek_result == NULL || seek_result->data == NULL || name == NULL) {
        return false;
    }

    uint8_t *data = seek_result->data;
    uint16_t len = seek_result->data_length;
    size_t name_len = strlen(name);
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
            memcmp(&data[i + 2U], name, name_len) == 0) {
            return true;
        }
        i = (uint16_t)(i + 2U + ad_len);
    }

    return false;
}

static bool adv_service_match(const sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL || seek_result->data == NULL) {
        return false;
    }

    uint8_t *data = seek_result->data;
    uint16_t len = seek_result->data_length;
    for (uint16_t i = 0; i < len;) {
        if ((i + 1U) >= len) {
            break;
        }

        uint8_t ad_type = data[i];
        uint8_t ad_len = data[i + 1U];
        if ((uint16_t)(i + 2U + ad_len) > len) {
            break;
        }

        if (ad_type == SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS && ad_len >= UUID_LEN_2) {
            for (uint8_t j = 0; (uint8_t)(j + 1U) < ad_len; j = (uint8_t)(j + UUID_LEN_2)) {
                uint16_t uuid = (uint16_t)data[i + 2U + j] | ((uint16_t)data[i + 3U + j] << 8);
                if (uuid == SLE_JOB_SERVICE_UUID) {
                    return true;
                }
            }
        }
        i = (uint16_t)(i + 2U + ad_len);
    }

    return false;
}

static bool seek_result_matches_receiver(const sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL) {
        return false;
    }

    if (memcmp(seek_result->addr.addr, g_receiver_mac, SLE_ADDR_LEN) == 0 ||
        memcmp(seek_result->addr.addr, g_test_receiver_mac, SLE_ADDR_LEN) == 0) {
        return true;
    }

    if (adv_name_match(seek_result, SLE_JOB_RECEIVER_NAME) || adv_name_match(seek_result, "LaserRX")) {
        return true;
    }

    return adv_service_match(seek_result);
}

static bool adv_panel_service_match(const sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL || seek_result->data == NULL) {
        return false;
    }

    uint8_t *data = seek_result->data;
    uint16_t len = seek_result->data_length;
    for (uint16_t i = 0; i < len;) {
        if ((i + 1U) >= len) {
            break;
        }

        uint8_t ad_type = data[i];
        uint8_t ad_len = data[i + 1U];
        if ((uint16_t)(i + 2U + ad_len) > len) {
            break;
        }

        if (ad_type == SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS && ad_len >= UUID_LEN_2) {
            for (uint8_t j = 0; (uint8_t)(j + 1U) < ad_len; j = (uint8_t)(j + UUID_LEN_2)) {
                uint16_t uuid = (uint16_t)data[i + 2U + j] | ((uint16_t)data[i + 3U + j] << 8);
                if (uuid == SLE_PANEL_SERVICE_UUID) {
                    return true;
                }
            }
        }
        i = (uint16_t)(i + 2U + ad_len);
    }

    return false;
}

static bool seek_result_matches_panel(const sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL) {
        return false;
    }
    return adv_name_match(seek_result, SLE_PANEL_SERVER_NAME) ||
           adv_name_match(seek_result, "WS63_PANEL") ||
           adv_panel_service_match(seek_result);
}

static bool need_rx_link(void)
{
    return g_conn_id == SLE_CONN_INVALID;
}

static bool need_panel_link(void)
{
    return g_background_seek_allowed &&
           g_panel_link_allowed &&
           g_panel_conn_id == SLE_CONN_INVALID;
}

static void start_seek_if_needed(void)
{
    if (!g_sle_enabled || g_seek_active || g_connecting ||
        (!need_rx_link() && !need_panel_link())) {
        return;
    }

    uint32_t now = (uint32_t)uapi_systick_get_ms();
    if (g_last_connect_ms != 0 &&
        (uint32_t)(now - g_last_connect_ms) < CONNECT_RETRY_INTERVAL_MS) {
        return;
    }

    sle_seek_param_t param = {0};
    param.own_addr_type = 0;
    param.filter_duplicates = 0;
    param.seek_filter_policy = 0;
    param.seek_phys = 1;
    param.seek_type[0] = SLE_SEEK_ACTIVE;
    param.seek_interval[0] = 0x64;
    param.seek_window[0] = 0x64;
    (void)sle_set_seek_param(&param);

    g_last_connect_ms = now;
    errcode_t ret = sle_start_seek();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[tx] start seek fail: 0x%x\r\n", ret);
    }
}

static void stop_seek_if_no_rx_link_needed(const char *reason)
{
    if (!g_seek_active || need_rx_link()) {
        return;
    }

    errcode_t ret = sle_stop_seek();
    osal_printk("[tx_seek] stop background seek reason=%s ret=0x%x\r\n",
                (reason != NULL) ? reason : "unspecified", ret);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[tx_seek] stop background seek fail: 0x%x\r\n", ret);
    }
}

/* SLE callbacks */
static void sle_enable_cbk(errcode_t status)
{
    if (status == ERRCODE_SLE_SUCCESS) {
        g_sle_enabled = true;
        start_seek_if_needed();
    } else {
        osal_printk("[tx] sle enable fail: 0x%x\r\n", status);
    }
}

static void seek_result_cbk(sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL || g_connecting) {
        return;
    }

    uint8_t role = SLE_LINK_ROLE_NONE;
    if (need_rx_link() && seek_result_matches_receiver(seek_result)) {
        role = SLE_LINK_ROLE_RX;
    } else if (need_panel_link() && seek_result_matches_panel(seek_result)) {
        role = SLE_LINK_ROLE_PANEL;
    }

    if (role == SLE_LINK_ROLE_NONE) {
        return;
    }

    memcpy_s(&g_pending_addr, sizeof(g_pending_addr), &seek_result->addr, sizeof(seek_result->addr));
    g_connecting = true;
    g_pending_role = role;
    errcode_t ret = sle_stop_seek();
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_connecting = false;
        g_pending_role = SLE_LINK_ROLE_NONE;
        osal_printk("[tx] stop seek fail: 0x%x\r\n", ret);
    }
}

static void seek_enable_cbk(errcode_t status)
{
    g_seek_active = (status == ERRCODE_SLE_SUCCESS);
}

static void seek_disable_cbk(errcode_t status)
{
    g_seek_active = false;
    if (status != ERRCODE_SLE_SUCCESS || !g_connecting) {
        g_connecting = false;
        g_pending_role = SLE_LINK_ROLE_NONE;
        start_seek_if_needed();
        return;
    }

    errcode_t ret = sle_connect_remote_device(&g_pending_addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_connecting = false;
        g_pending_role = SLE_LINK_ROLE_NONE;
        osal_printk("[tx] connect request submit fail: 0x%x\r\n", ret);
        start_seek_if_needed();
    }
}

static void connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        uint8_t role = g_pending_role;
        if (role == SLE_LINK_ROLE_NONE) {
            if (need_rx_link()) {
                role = SLE_LINK_ROLE_RX;
            } else if (need_panel_link()) {
                role = SLE_LINK_ROLE_PANEL;
            }
        }
        if (role == SLE_LINK_ROLE_NONE) {
            g_connecting = false;
            g_pending_role = SLE_LINK_ROLE_NONE;
            if (addr != NULL) {
                (void)sle_disconnect_remote_device(addr);
            }
            start_seek_if_needed();
            return;
        }
        if (role == SLE_LINK_ROLE_PANEL) {
            if (!g_panel_link_allowed) {
                g_connecting = false;
                g_pending_role = SLE_LINK_ROLE_NONE;
                osal_printk("[tx_panel] reject link while disabled\r\n");
                if (addr != NULL) {
                    (void)sle_disconnect_remote_device(addr);
                }
                start_seek_if_needed();
                return;
            }
            g_panel_conn_id = conn_id;
            if (addr != NULL) {
                (void)memcpy_s(&g_panel_addr, sizeof(g_panel_addr), addr, sizeof(*addr));
                g_panel_addr_valid = true;
            } else {
                memset(&g_panel_addr, 0, sizeof(g_panel_addr));
                g_panel_addr_valid = false;
            }
        } else {
            g_conn_id = conn_id;
        }
        g_connecting = false;
        g_pending_role = SLE_LINK_ROLE_NONE;

        /* Request fast connection interval immediately. Unit is 1.25ms. */
        sle_connection_param_update_t parame = {0};
        parame.conn_id = conn_id;
        parame.interval_min = JOB_SLE_CONN_INTERVAL_UNITS;
        parame.interval_max = JOB_SLE_CONN_INTERVAL_UNITS;
        parame.max_latency = 0;
        parame.supervision_timeout = 0x1F4;
        errcode_t param_ret = sle_update_connect_param(&parame);
        osal_printk("[tx_conn_param] conn=%u interval_units=0x%02x ret=0x%x\r\n",
                    (unsigned int)conn_id,
                    (unsigned int)JOB_SLE_CONN_INTERVAL_UNITS,
                    (unsigned int)param_ret);

        /* Start service discovery */
        ssap_exchange_info_t info = {0};
        info.mtu_size = JOB_SLE_MTU_SIZE;
        info.version = 1;
        ssapc_exchange_info_req(SLE_CLIENT_ID, conn_id, &info);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        if (conn_id == g_panel_conn_id) {
            g_panel_conn_id = SLE_CONN_INVALID;
            g_panel_handles_ready = false;
            g_panel_status_handle = 0;
            memset(&g_panel_addr, 0, sizeof(g_panel_addr));
            g_panel_addr_valid = false;
            osal_printk("[tx_panel] disconnected\r\n");
        }
        if (conn_id == g_conn_id) {
            g_conn_id = SLE_CONN_INVALID;
            g_handles_ready = false;
            g_data_handle = 0;
            g_resp_handle = 0;
            osal_printk("[tx] rx disconnected\r\n");
        }
        g_connecting = false;
        g_pending_role = SLE_LINK_ROLE_NONE;
        start_seek_if_needed();
    }
}

static void pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    unused(status);
}

static void auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr,
    errcode_t status, const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    unused(status);
}

static void exchange_info_cbk(uint8_t client_id, uint16_t conn_id,
    ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    unused(param);
    unused(status);
    /* Discover services */
    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    (void)ssapc_find_structure(SLE_CLIENT_ID, conn_id, &find_param);
}

static void find_structure_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_service_result_t *service, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || service == NULL) return;

    bool is_panel_conn = (conn_id == g_panel_conn_id);
    uint16_t expected_uuid = is_panel_conn ? SLE_PANEL_SERVICE_UUID : SLE_JOB_SERVICE_UUID;
    if (!uuid16_equals(&service->uuid, expected_uuid)) return;

    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PROPERTY;
    find_param.start_hdl = service->start_hdl;
    find_param.end_hdl = service->end_hdl;
    (void)ssapc_find_structure(SLE_CLIENT_ID, conn_id, &find_param);
}

static void find_property_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_property_result_t *property, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || property == NULL) return;

    if (conn_id == g_panel_conn_id) {
        if (uuid16_equals(&property->uuid, SLE_PANEL_STATUS_CHAR_UUID)) {
            g_panel_status_handle = property->handle;
            g_panel_handles_ready = true;
            start_seek_if_needed();
        }
        return;
    }

    if (conn_id != g_conn_id) {
        return;
    }

    if (uuid16_equals(&property->uuid, SLE_JOB_DATA_CHAR_UUID)) {
        g_data_handle = property->handle;
    } else if (uuid16_equals(&property->uuid, SLE_JOB_RESP_CHAR_UUID)) {
        g_resp_handle = property->handle;
    }

    g_handles_ready = (g_data_handle != 0) && (g_resp_handle != 0);
    if (g_handles_ready) {
        start_seek_if_needed();
    }
}

static void write_cfm_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_write_result_t *write_result, errcode_t status)
{
    unused(client_id);
    unused(write_result);
    if (conn_id != g_conn_id) {
        return;
    }
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[tx] write cfm fail: 0x%x\r\n", status);
    }
    g_last_write_cfm_status = status;
    if (g_write_cfm_sem_ready) {
        osal_sem_up(&g_write_cfm_sem);
    }
}

static void notification_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);

    if (status != ERRCODE_SLE_SUCCESS || data == NULL || data->data == NULL) {
        return;
    }

    if (conn_id == g_conn_id && data->handle == g_resp_handle) {
        trace_response_notification(conn_id, data->handle, data);
        if (g_response_cb != NULL) {
            g_response_cb(data->data, data->data_len);
        }
    }
}

static void indication_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    notification_cbk(client_id, conn_id, data, status);
}

#if JOB_TX_DATA_USE_WRITE_CMD
static uint8_t packet_type_from_encoded(const void *data, uint16_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (bytes == NULL || len < SLE_JOB_PACKET_HEADER_LEN) {
        return 0;
    }
    return bytes[2];
}
#endif

/* Public API */
errcode_t sle_job_client_init(void)
{
    g_conn_id = SLE_CONN_INVALID;
    g_panel_conn_id = SLE_CONN_INVALID;
    g_panel_link_allowed = true;
    g_background_seek_allowed = true;
    g_sle_enabled = false;
    g_seek_active = false;
    g_connecting = false;
    g_pending_role = SLE_LINK_ROLE_NONE;
    memset(&g_pending_addr, 0, sizeof(g_pending_addr));
    memset(&g_panel_addr, 0, sizeof(g_panel_addr));
    g_panel_addr_valid = false;
    g_handles_ready = false;
    g_panel_handles_ready = false;
    g_data_handle = 0;
    g_resp_handle = 0;
    g_panel_status_handle = 0;
    g_last_connect_ms = 0;
    g_last_write_cfm_status = ERRCODE_SLE_FAIL;
    if (!g_write_cfm_sem_ready && osal_sem_init(&g_write_cfm_sem, 0) == OSAL_SUCCESS) {
        g_write_cfm_sem_ready = true;
    }

    /* Register callbacks - minimal set for fast connection */
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
    ssapc_cbk.write_cfm_cb = write_cfm_cbk;
    ssapc_cbk.notification_cb = notification_cbk;
    ssapc_cbk.indication_cb = indication_cbk;
    ssapc_register_callbacks(&ssapc_cbk);

    errcode_t ret = enable_sle();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[tx] enable_sle fail: 0x%x\r\n", ret);
    }
    return ret;
}

errcode_t sle_job_client_send_packet_ex(const void *data, uint16_t len, bool force_write_req)
{
    if (data == NULL || len == 0 || !g_handles_ready || g_data_handle == 0) {
        return ERRCODE_SLE_FAIL;
    }

    ssapc_write_param_t param = {0};
    param.handle = g_data_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = len;
    param.data = (uint8_t *)data;

#if JOB_TX_DATA_USE_WRITE_CMD
    uint8_t pkt_type = packet_type_from_encoded(data, len);
    if (pkt_type == PKT_JOB_DATA && !force_write_req) {
        uint32_t t_write = (uint32_t)uapi_systick_get_ms();
        errcode_t ret = ssapc_write_cmd(SLE_CLIENT_ID, g_conn_id, &param);
        uint32_t call_ms = (uint32_t)uapi_systick_get_ms() - t_write;
        if (ret != ERRCODE_SLE_SUCCESS || call_ms >= JOB_SLE_WRITE_CALL_SLOW_MS) {
            osal_printk("[JOB_SLE_WRITE_CMD_TIMING] ret=0x%x len=%u call_ms=%u conn=%u handle=0x%x\r\n",
                        ret, (unsigned int)len, (unsigned int)call_ms,
                        (unsigned int)g_conn_id, (unsigned int)g_data_handle);
        }
        return ret;
    }
#endif

    if (!g_write_cfm_sem_ready) {
        return ERRCODE_SLE_FAIL;
    }
    while (osal_sem_down_timeout(&g_write_cfm_sem, 0) == OSAL_SUCCESS) {
    }

    g_last_write_cfm_status = ERRCODE_SLE_FAIL;
    if (JOB_DIAG_LOG) {
        osal_printk("[JOB_SLE_WRITE_CALL] conn=%u handle=0x%x len=%u\r\n",
                    (unsigned int)g_conn_id, (unsigned int)g_data_handle, (unsigned int)len);
    }
    uint32_t t_write = (uint32_t)uapi_systick_get_ms();
    errcode_t ret = ssapc_write_req(SLE_CLIENT_ID, g_conn_id, &param);
    uint32_t call_ms = (uint32_t)uapi_systick_get_ms() - t_write;
    if (JOB_DIAG_LOG || ret != ERRCODE_SLE_SUCCESS || call_ms >= JOB_SLE_WRITE_CALL_SLOW_MS) {
        osal_printk("[JOB_SLE_WRITE_RET] ret=0x%x len=%u call_ms=%u force_req=%u\r\n",
                    ret, (unsigned int)len, (unsigned int)call_ms,
                    (unsigned int)(force_write_req ? 1U : 0U));
    }
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }
    uint32_t t_cfm = (uint32_t)uapi_systick_get_ms();
    if (osal_sem_down_timeout(&g_write_cfm_sem, JOB_SLE_WRITE_CFM_TIMEOUT_MS) != OSAL_SUCCESS) {
        osal_printk("[JOB_SLE_WRITE_CFM_TIMEOUT] len=%u timeout=%u force_req=%u\r\n",
                    (unsigned int)len, (unsigned int)JOB_SLE_WRITE_CFM_TIMEOUT_MS,
                    (unsigned int)(force_write_req ? 1U : 0U));
        return ERRCODE_SLE_TIMEOUT;
    }
    uint32_t cfm_wait_ms = (uint32_t)uapi_systick_get_ms() - t_cfm;
    uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_write;

    if (JOB_DIAG_LOG || g_last_write_cfm_status != ERRCODE_SLE_SUCCESS ||
        call_ms >= JOB_SLE_WRITE_CALL_SLOW_MS || cfm_wait_ms >= 100U) {
        osal_printk("[JOB_SLE_WRITE_CFM] status=0x%x len=%u call_ms=%u cfm_wait_ms=%u "
                    "total_ms=%u force_req=%u\r\n",
                    g_last_write_cfm_status, (unsigned int)len,
                    (unsigned int)call_ms, (unsigned int)cfm_wait_ms,
                    (unsigned int)total_ms, (unsigned int)(force_write_req ? 1U : 0U));
    }
    return g_last_write_cfm_status;
}

errcode_t sle_job_client_send_packet(const void *data, uint16_t len)
{
    return sle_job_client_send_packet_ex(data, len, false);
}

bool sle_job_client_is_connected(void)
{
    return g_conn_id != SLE_CONN_INVALID && g_handles_ready;
}

bool sle_job_client_panel_is_connected(void)
{
    return g_panel_link_allowed && g_panel_conn_id != SLE_CONN_INVALID && g_panel_handles_ready;
}

bool sle_job_client_panel_link_allowed(void)
{
    return g_panel_link_allowed;
}

void sle_job_client_set_panel_link_allowed(bool allowed)
{
    if (g_panel_link_allowed == allowed) {
        if (!allowed) {
            stop_seek_if_no_rx_link_needed("panel-disabled");
        }
        return;
    }

    g_panel_link_allowed = allowed;
    osal_printk("[tx_panel] link_allowed=%u\r\n", allowed ? 1U : 0U);

    if (!allowed) {
        if (g_pending_role == SLE_LINK_ROLE_PANEL) {
            g_connecting = false;
            g_pending_role = SLE_LINK_ROLE_NONE;
            memset(&g_pending_addr, 0, sizeof(g_pending_addr));
        }
        if (g_panel_conn_id != SLE_CONN_INVALID && g_panel_addr_valid) {
            errcode_t ret = sle_disconnect_remote_device(&g_panel_addr);
            if (ret != ERRCODE_SLE_SUCCESS) {
                osal_printk("[tx_panel] disconnect submit fail: 0x%x\r\n", ret);
            }
        }
        stop_seek_if_no_rx_link_needed("panel-disabled");
        return;
    }

    start_seek_if_needed();
}

void sle_job_client_set_background_seek_allowed(bool allowed)
{
    if (g_background_seek_allowed == allowed) {
        if (!allowed) {
            stop_seek_if_no_rx_link_needed("background-disabled");
        }
        return;
    }

    g_background_seek_allowed = allowed;
    osal_printk("[tx_seek] background_allowed=%u\r\n", allowed ? 1U : 0U);

    if (!allowed) {
        if (g_pending_role == SLE_LINK_ROLE_PANEL) {
            g_connecting = false;
            g_pending_role = SLE_LINK_ROLE_NONE;
            memset(&g_pending_addr, 0, sizeof(g_pending_addr));
        }
        stop_seek_if_no_rx_link_needed("background-disabled");
        return;
    }

    start_seek_if_needed();
}

errcode_t sle_job_client_mirror_panel_packet(const void *data, uint16_t len)
{
    if (data == NULL || len == 0 || !g_panel_link_allowed ||
        !sle_job_client_panel_is_connected() ||
        g_panel_status_handle == 0) {
        return ERRCODE_SLE_FAIL;
    }

    ssapc_write_param_t param = {0};
    param.handle = g_panel_status_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = len;
    param.data = (uint8_t *)data;

    errcode_t ret = ssapc_write_cmd(SLE_CLIENT_ID, g_panel_conn_id, &param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[tx_panel] mirror write fail ret=0x%x len=%u\r\n",
                    ret, (unsigned int)len);
    }
    return ret;
}

void sle_job_client_poll_connect(void)
{
    start_seek_if_needed();
}

const char *sle_job_client_get_status(void)
{
    if (g_conn_id == SLE_CONN_INVALID) return g_connecting ? "connecting" : "disconnected";
    return g_handles_ready ? "ready" : "discovering";
}

void sle_job_client_set_response_cb(sle_job_response_cb_t cb)
{
    g_response_cb = cb;
}
