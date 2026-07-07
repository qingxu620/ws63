/**
 * @file panel_transport_sle.c
 * @brief SLE server for TX-mirrored status and SLE client for offline RX jobs.
 */
#include "panel_transport_sle.h"
#include "panel_model.h"
#include "task_manager.h"

#include "common_def.h"
#include "errcode.h"
#include "packet.h"
#include "protocol.h"
#include "securec.h"
#include "soc_osal.h"
#include "systick.h"

#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"
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

#ifndef SLE_SEEK_ACTIVE
#define SLE_SEEK_ACTIVE 0x01
#endif

#ifndef SLE_SEEK_PASSIVE
#define SLE_SEEK_PASSIVE 0x00
#endif

#define PANEL_SLE_CONN_INVALID 0xFFFF
#define PANEL_SLE_CLIENT_ID 0
#define PANEL_SLE_UUID_LEN_2 2
#define PANEL_SLE_CONNECT_RETRY_MS 1000U
#define PANEL_SLE_WRITE_CFM_TIMEOUT_MS 1000U
#define PANEL_SLE_FAST_CONN_INTERVAL 0x14U
#define PANEL_SLE_ONLINE_CONN_INTERVAL 0x20U
#define PANEL_SLE_FAST_CONN_TIMEOUT  0x1F4U
#define PANEL_SLE_STATUS_LISTEN_PERIOD_MS 1000U
#define PANEL_SLE_STATUS_LISTEN_WINDOW_MS 1200U
#define PANEL_SLE_STATUS_LISTEN_TASK_STACK_SIZE 0x1000
#define PANEL_SLE_STATUS_LISTEN_TASK_PRIORITY 26

static const uint8_t g_panel_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x02};
static const uint8_t g_tx_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x03};
static uint8_t g_server_id = 0;
static uint16_t g_panel_conn_id = PANEL_SLE_CONN_INVALID;
static uint16_t g_rx_conn_id = PANEL_SLE_CONN_INVALID;
static uint16_t g_service_handle = 0;
static uint16_t g_status_property_handle = 0;
static uint16_t g_rx_data_handle = 0;
static uint16_t g_rx_resp_handle = 0;
static bool g_sle_enabled;
static bool g_seek_active;
static bool g_client_connecting;
static bool g_seek_for_status_listen;
static bool g_rx_handles_ready;
static sle_addr_t g_pending_rx_addr = {0};
static sle_addr_t g_rx_addr = {0};
static bool g_rx_addr_valid;
static bool g_rx_disconnect_expected;
static bool g_rx_connect_cancel_after_connect;
static uint32_t g_last_seek_ms;
static uint32_t g_status_listen_last_ms;
static uint32_t g_status_listen_started_ms;
static osal_semaphore g_write_cfm_sem;
static osal_mutex g_write_mutex;
static volatile bool g_write_cfm_sem_ready;
static volatile bool g_write_mutex_ready;
static volatile errcode_t g_last_write_cfm_status = ERRCODE_SLE_FAIL;
static volatile bool g_status_listen_task_started;
static panel_transport_rx_response_cb_t g_offline_response_cb;
static panel_transport_rx_response_cb_t g_cmd_response_cb;
static volatile bool g_standalone_session_active;

static const uint8_t g_receiver_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x01};

static uint8_t packet_type_from_encoded(const void *data, uint16_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (bytes == NULL || len < SLE_JOB_PACKET_HEADER_LEN) {
        return 0;
    }
    return bytes[2];
}

static uint8_t sle_uuid_base[] = {
    0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t g_announce_data[] = {
    SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
    1,
    SLE_ANNOUNCE_LEVEL_NORMAL,
    SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS,
    2,
    (uint8_t)(SLE_PANEL_SERVICE_UUID & 0xFF),
    (uint8_t)(SLE_PANEL_SERVICE_UUID >> 8),
};

static uint8_t g_scan_rsp_data[] = {
    SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
    1,
    20,
    SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME,
    sizeof(SLE_PANEL_SERVER_NAME) - 1,
    'w', 's', '6', '3', '_', 'p', 'a', 'n', 'e', 'l'
};

static void request_fast_rx_conn_params(uint16_t conn_id)
{
    sle_connection_param_update_t param = {0};
    param.conn_id = conn_id;
    param.interval_min = PANEL_SLE_FAST_CONN_INTERVAL;
    param.interval_max = PANEL_SLE_FAST_CONN_INTERVAL;
    param.max_latency = 0;
    param.supervision_timeout = PANEL_SLE_FAST_CONN_TIMEOUT;

    errcode_t ret = sle_update_connect_param(&param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_CLI] fast conn param update fail: 0x%x\r\n", ret);
    }
}

static void sle_uuid_set_base(sle_uuid_t *out)
{
    (void)memcpy_s(out->uuid, SLE_UUID_LEN, sle_uuid_base, SLE_UUID_LEN);
    out->len = 2;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    sle_uuid_set_base(out);
    out->uuid[14] = (uint8_t)(u2 & 0xFF);
    out->uuid[15] = (uint8_t)((u2 >> 8) & 0xFF);
}

static bool uuid16_equals(const sle_uuid_t *uuid, uint16_t expect)
{
    return (uuid != NULL) && (uuid->len == PANEL_SLE_UUID_LEN_2) &&
           (uuid->uuid[14] == (uint8_t)(expect & 0xFF)) &&
           (uuid->uuid[15] == (uint8_t)((expect >> 8) & 0xFF));
}

static bool adv_panel_status_parse(const sle_seek_result_info_t *seek_result,
                                   panel_status_payload_t *status)
{
    if (seek_result == NULL || seek_result->data == NULL || status == NULL) {
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
        if (ad_type == SLE_ADV_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA &&
            ad_len == sizeof(panel_status_adv_payload_t)) {
            panel_status_adv_payload_t adv;
            (void)memcpy_s(&adv, sizeof(adv), &data[i + 2U], sizeof(adv));
            if (adv.magic0 == PANEL_STATUS_ADV_MAGIC0 &&
                adv.magic1 == PANEL_STATUS_ADV_MAGIC1 &&
                adv.version == PANEL_STATUS_ADV_VERSION) {
                *status = adv.status;
                return true;
            }
        }
        i = (uint16_t)(i + 2U + ad_len);
    }
    return false;
}

static bool addr_matches_mac(const sle_addr_t *addr, const uint8_t *mac)
{
    if (addr == NULL || mac == NULL) {
        return false;
    }
    return memcmp(addr->addr, mac, SLE_ADDR_LEN) == 0;
}

static bool seek_result_matches_receiver(const sle_seek_result_info_t *seek_result)
{
    return seek_result != NULL && addr_matches_mac(&seek_result->addr, g_receiver_mac);
}

static bool seek_result_matches_tx(const sle_seek_result_info_t *seek_result)
{
    return seek_result != NULL && addr_matches_mac(&seek_result->addr, g_tx_mac);
}

static bool rx_control_allowed_now(void)
{
    /*
     * Normal product topology is Host/TX -> RX plus TX -> Screen mirror.
     * The panel must not keep a background RX client link; it may connect to
     * RX only while an explicit standalone/offline control session is active.
     */
    return g_standalone_session_active;
}

static bool addr_matches_pending_rx(const sle_addr_t *addr)
{
    return addr_matches_mac(addr, g_pending_rx_addr.addr);
}

static void disconnect_rx_expected(void)
{
    if (!g_rx_addr_valid) {
        return;
    }

    g_rx_disconnect_expected = true;
    errcode_t ret = sle_disconnect_remote_device(&g_rx_addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_rx_disconnect_expected = false;
        osal_printk("[PANEL_SLE_CLI] rx disconnect submit fail: 0x%x\r\n", ret);
    }
}

static void start_seek_if_needed(void)
{
    if (!g_sle_enabled || !rx_control_allowed_now() ||
        g_seek_active || g_client_connecting ||
        (g_rx_conn_id != PANEL_SLE_CONN_INVALID && g_rx_handles_ready)) {
        return;
    }

    uint32_t now = (uint32_t)uapi_systick_get_ms();
    if (g_last_seek_ms != 0U &&
        (uint32_t)(now - g_last_seek_ms) < PANEL_SLE_CONNECT_RETRY_MS) {
        return;
    }

    sle_seek_param_t param = {0};
    param.own_addr_type = 0;
    param.filter_duplicates = 1;
    param.seek_filter_policy = 0;
    param.seek_phys = 1;
    param.seek_type[0] = SLE_SEEK_ACTIVE;
    param.seek_interval[0] = 0x64;
    param.seek_window[0] = 0x64;
    (void)sle_set_seek_param(&param);

    g_last_seek_ms = now;
    errcode_t ret = sle_start_seek();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_CLI] start seek fail: 0x%x\r\n", ret);
    }
}

static void start_status_listen_if_needed(void)
{
    if (!g_sle_enabled || rx_control_allowed_now() || g_seek_active || g_client_connecting ||
        g_rx_conn_id != PANEL_SLE_CONN_INVALID) {
        return;
    }

    uint32_t now = (uint32_t)uapi_systick_get_ms();
    if (g_status_listen_last_ms != 0U &&
        (uint32_t)(now - g_status_listen_last_ms) < PANEL_SLE_STATUS_LISTEN_PERIOD_MS) {
        return;
    }

    sle_seek_param_t param = {0};
    param.own_addr_type = 0;
    param.filter_duplicates = 1;
    param.seek_filter_policy = 0;
    param.seek_phys = 1;
    param.seek_type[0] = SLE_SEEK_ACTIVE;
    param.seek_interval[0] = 0x64;
    param.seek_window[0] = 0x64;
    (void)sle_set_seek_param(&param);

    g_status_listen_last_ms = now;
    g_status_listen_started_ms = now;
    g_seek_for_status_listen = true;
    errcode_t ret = sle_start_seek();
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_seek_for_status_listen = false;
        static uint32_t s_listen_start_fail_count = 0;
        if ((s_listen_start_fail_count++ & 0x0FU) == 0U) {
            osal_printk("[PANEL_SLE_LISTEN] start seek fail: 0x%x\r\n", ret);
        }
    } else {
        static uint32_t s_listen_start_count = 0;
        if ((s_listen_start_count++ & 0x0FU) == 0U) {
            osal_printk("[PANEL_SLE_LISTEN] start\r\n");
        }
    }
}

static void stop_status_listen_if_expired(void)
{
    if (!g_seek_for_status_listen || !g_seek_active) {
        return;
    }

    uint32_t now = (uint32_t)uapi_systick_get_ms();
    if ((uint32_t)(now - g_status_listen_started_ms) < PANEL_SLE_STATUS_LISTEN_WINDOW_MS) {
        return;
    }

    errcode_t ret = sle_stop_seek();
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_seek_for_status_listen = false;
        static uint32_t s_listen_stop_fail_count = 0;
        if ((s_listen_stop_fail_count++ & 0x0FU) == 0U) {
            osal_printk("[PANEL_SLE_LISTEN] stop seek fail: 0x%x\r\n", ret);
        }
    }
}

static int panel_status_listen_task(void *arg)
{
    unused(arg);
    while (1) {
        stop_status_listen_if_expired();
        start_status_listen_if_needed();
        osal_msleep(20);
    }
    return 0;
}

static void start_status_listen_task_once(void)
{
    if (g_status_listen_task_started) {
        return;
    }
    errcode_t ret = task_create("panel_sle_listen", panel_status_listen_task, NULL,
                                PANEL_SLE_STATUS_LISTEN_TASK_STACK_SIZE,
                                PANEL_SLE_STATUS_LISTEN_TASK_PRIORITY);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[PANEL_SLE_LISTEN] create task failed: 0x%x\r\n", ret);
        return;
    }
    g_status_listen_task_started = true;
}

static uint8_t panel_mode_from_job_state(uint8_t job_state, uint8_t status)
{
    if (status != JOB_STATUS_OK || job_state == JOB_STATE_ERROR) {
        return PANEL_MODE_ERROR;
    }
    return PANEL_MODE_ONLINE;
}

static void apply_status_resp_as(const status_resp_payload_t *st, uint8_t owner, uint8_t mode)
{
    if (st == NULL) {
        return;
    }

    uint8_t flags = PANEL_STATUS_FLAG_OWNER_LINK | PANEL_STATUS_FLAG_ANY_LINK;
    uint8_t effective_mode = (st->status == JOB_STATUS_OK && st->state != JOB_STATE_ERROR) ?
        mode : PANEL_MODE_ERROR;
    panel_model_apply_rx_panel_status(owner, effective_mode, st->state, flags,
                                      g_model.seq + 1U, st->job_id,
                                      st->received_size, st->total_size,
                                      st->executed_lines, st->cache_free,
                                      st->status, 0U);
}

static void apply_panel_status(const panel_status_payload_t *st)
{
    if (st == NULL) {
        return;
    }
    uint8_t flags = st->flags | PANEL_STATUS_FLAG_ANY_LINK;
    panel_model_apply_rx_panel_status(st->owner, st->mode, st->job_state, flags, st->seq,
                                      st->job_id, st->received_size, st->total_size,
                                      st->executed_lines, st->cache_free, st->last_error,
                                      st->tick_ms);
}

static void handle_panel_write(const uint8_t *data, uint16_t len)
{
    sle_packet_view_t pkt;
    if (data == NULL || len == 0 || !sle_packet_decode(data, len, &pkt)) {
        return;
    }

    if (pkt.type == PKT_PANEL_STATUS && pkt.len == sizeof(panel_status_payload_t)) {
        panel_status_payload_t st;
        (void)memcpy_s(&st, sizeof(st), pkt.payload, sizeof(st));
        apply_panel_status(&st);
        return;
    }

    if (pkt.type == PKT_STATUS_RESP && pkt.len == sizeof(status_resp_payload_t)) {
        status_resp_payload_t st;
        (void)memcpy_s(&st, sizeof(st), pkt.payload, sizeof(st));
        apply_status_resp_as(&st, PANEL_OWNER_HOST, panel_mode_from_job_state(st.state, st.status));
    }
}

static void ssaps_write_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    if (status != ERRCODE_SLE_SUCCESS || write_cb_para == NULL ||
        write_cb_para->value == NULL || write_cb_para->length == 0) {
        return;
    }

    if (write_cb_para->handle != g_status_property_handle) {
        return;
    }

    g_panel_conn_id = conn_id;
    panel_model_set_transport_links(panel_transport_sle_tx_is_connected(),
                                    panel_transport_sle_rx_is_connected());
    handle_panel_write(write_cb_para->value, write_cb_para->length);
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
    unused(mtu_size);
    unused(status);
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

static errcode_t panel_add_service(void)
{
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_PANEL_SERVICE_UUID, &service_uuid);
    return ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
}

static errcode_t panel_add_status_property(void)
{
    ssaps_property_info_t property = {0};
    uint8_t init_val[SLE_JOB_PACKET_MAX_SIZE] = {0};

    property.permissions = 0x01 | 0x02;
    sle_uuid_setu2(SLE_PANEL_STATUS_CHAR_UUID, &property.uuid);
    property.value = init_val;
    property.value_len = sizeof(init_val);
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_WRITE | SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP;
    return ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_status_property_handle);
}

static errcode_t panel_server_add(void)
{
    sle_uuid_t app_uuid = {0};
    char app_uuid_data[] = {0x0, 0x0};
    app_uuid.len = sizeof(app_uuid_data);
    (void)memcpy_s(app_uuid.uuid, app_uuid.len, app_uuid_data, sizeof(app_uuid_data));

    errcode_t ret = ssaps_register_server(&app_uuid, &g_server_id);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] register server fail: 0x%x\r\n", ret);
        return ret;
    }
    ret = panel_add_service();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] add service fail: 0x%x\r\n", ret);
        return ret;
    }
    ret = panel_add_status_property();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] add status property fail: 0x%x\r\n", ret);
        return ret;
    }
    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] start service fail: 0x%x\r\n", ret);
    }
    return ret;
}

static void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        if (g_client_connecting && addr_matches_pending_rx(addr)) {
            g_rx_conn_id = conn_id;
            g_client_connecting = false;
            if (addr != NULL) {
                (void)memcpy_s(&g_rx_addr, sizeof(g_rx_addr), addr, sizeof(*addr));
                g_rx_addr_valid = true;
            }
            request_fast_rx_conn_params(conn_id);
            if (g_rx_connect_cancel_after_connect && !g_standalone_session_active) {
                g_rx_connect_cancel_after_connect = false;
                disconnect_rx_expected();
                return;
            }
            ssap_exchange_info_t info = {0};
            info.mtu_size = 512;
            info.version = 1;
            (void)ssapc_exchange_info_req(PANEL_SLE_CLIENT_ID, conn_id, &info);
        } else if (addr_matches_mac(addr, g_tx_mac)) {
            g_panel_conn_id = conn_id;
            if (g_client_connecting && !g_standalone_session_active) {
                g_rx_connect_cancel_after_connect = true;
            }
            if (!g_standalone_session_active && g_rx_addr_valid) {
                disconnect_rx_expected();
            }
            panel_model_set_transport_links(true, panel_transport_sle_rx_is_connected());
            osal_printk("[PANEL_SLE_SRV] tx mirror connected conn=%u\r\n",
                        (unsigned int)conn_id);
        } else {
            osal_printk("[PANEL_SLE_SRV] reject non-whitelist peer conn=%u\r\n",
                        (unsigned int)conn_id);
            if (addr != NULL) {
                (void)sle_disconnect_remote_device(addr);
            }
        }
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        if (conn_id == g_panel_conn_id) {
            g_panel_conn_id = PANEL_SLE_CONN_INVALID;
            panel_model_set_transport_links(false, panel_transport_sle_rx_is_connected());
        }
        if (conn_id == g_rx_conn_id) {
            bool expected = g_rx_disconnect_expected;
            g_rx_disconnect_expected = false;
            g_rx_conn_id = PANEL_SLE_CONN_INVALID;
            g_rx_handles_ready = false;
            g_rx_data_handle = 0;
            g_rx_resp_handle = 0;
            g_rx_connect_cancel_after_connect = false;
            memset(&g_rx_addr, 0, sizeof(g_rx_addr));
            g_rx_addr_valid = false;
            osal_printk("[PANEL_SLE_CLI] rx disconnected\r\n");
            panel_model_set_transport_links(panel_transport_sle_tx_is_connected(), false);
            if (expected) {
                start_seek_if_needed();
                return;
            }
        }
        g_client_connecting = false;
        if (!panel_transport_sle_tx_is_connected() && !panel_transport_sle_rx_is_connected()) {
            panel_model_apply_rx_panel_status(PANEL_OWNER_NONE, PANEL_MODE_LINK_LOST, JOB_STATE_IDLE,
                                              0, g_model.seq + 1U, 0, 0, 0, 0, 0, 1, 0U);
        }
        start_seek_if_needed();
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
    sle_connection_register_callbacks(&cbk);
}

static void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    unused(status);
}

static void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    unused(status);
}

static void sle_announce_terminal_cbk(uint32_t announce_id)
{
    unused(announce_id);
}

static void sle_seek_enable_cbk(errcode_t status)
{
    g_seek_active = (status == ERRCODE_SLE_SUCCESS);
}

static void sle_seek_result_cbk(sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL) {
        return;
    }

    bool matches_rx = seek_result_matches_receiver(seek_result);
    bool matches_tx = seek_result_matches_tx(seek_result);

    if (g_seek_for_status_listen && (matches_rx || matches_tx)) {
        panel_status_payload_t status;
        if (adv_panel_status_parse(seek_result, &status)) {
            apply_panel_status(&status);
            static uint32_t s_status_listen_count = 0;
            if ((s_status_listen_count++ & 0x1FU) == 0U) {
                osal_printk("[PANEL_SLE_LISTEN] status seq=%u owner=%u state=%u rx=%u total=%u\r\n",
                            (unsigned int)status.seq, (unsigned int)status.owner,
                            (unsigned int)status.job_state,
                            (unsigned int)status.received_size,
                            (unsigned int)status.total_size);
            }
            if (g_seek_for_status_listen && !rx_control_allowed_now()) {
                errcode_t ret = sle_stop_seek();
                if (ret != ERRCODE_SLE_SUCCESS) {
                    g_seek_for_status_listen = false;
                }
                return;
            }
        }
    }

    if (matches_rx && g_seek_for_status_listen) {
        panel_status_payload_t status;
        if (!adv_panel_status_parse(seek_result, &status)) {
            static uint32_t s_status_parse_miss_count = 0;
            if ((s_status_parse_miss_count++ & 0x0FU) == 0U) {
                osal_printk("[PANEL_SLE_LISTEN] rx adv no status len=%u\r\n",
                            (unsigned int)seek_result->data_length);
            }
        }
    }

    if (matches_tx && g_seek_for_status_listen) {
        panel_status_payload_t status;
        if (!adv_panel_status_parse(seek_result, &status)) {
            static uint32_t s_tx_status_parse_miss_count = 0;
            if ((s_tx_status_parse_miss_count++ & 0x0FU) == 0U) {
                osal_printk("[PANEL_SLE_LISTEN] tx adv no status len=%u\r\n",
                            (unsigned int)seek_result->data_length);
            }
        }
    }

    if (g_client_connecting || g_rx_conn_id != PANEL_SLE_CONN_INVALID ||
        !rx_control_allowed_now() || !matches_rx) {
        return;
    }

    (void)memcpy_s(&g_pending_rx_addr, sizeof(g_pending_rx_addr),
                   &seek_result->addr, sizeof(seek_result->addr));
    g_client_connecting = true;
    errcode_t ret = sle_stop_seek();
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_client_connecting = false;
        osal_printk("[PANEL_SLE_CLI] stop seek fail: 0x%x\r\n", ret);
    }
}

static void sle_seek_disable_cbk(errcode_t status)
{
    bool was_status_listen = g_seek_for_status_listen;
    g_seek_for_status_listen = false;
    g_seek_active = false;
    if (was_status_listen && !g_client_connecting) {
        return;
    }
    if (status != ERRCODE_SLE_SUCCESS || !g_client_connecting) {
        g_client_connecting = false;
        g_rx_connect_cancel_after_connect = false;
        start_seek_if_needed();
        return;
    }

    errcode_t ret = sle_connect_remote_device(&g_pending_rx_addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_client_connecting = false;
        g_rx_connect_cancel_after_connect = false;
        osal_printk("[PANEL_SLE_CLI] connect submit fail: 0x%x\r\n", ret);
        start_seek_if_needed();
    }
}

static void sle_enable_cbk(errcode_t status)
{
    if (status != ERRCODE_SLE_SUCCESS) {
        return;
    }
    g_sle_enabled = true;

    errcode_t ret = panel_server_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        return;
    }

    ssap_exchange_info_t info = {0};
    info.mtu_size = 512;
    info.version = 1;
    ret = ssaps_set_info(g_server_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] set ssap info fail: 0x%x\r\n", ret);
        return;
    }

    sle_addr_t addr = {0};
    addr.type = 0;
    (void)memcpy_s(addr.addr, SLE_ADDR_LEN, g_panel_mac, SLE_ADDR_LEN);
    sle_set_local_addr(&addr);

    sle_announce_param_t param = {0};
    param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
    param.announce_handle = SLE_ADV_HANDLE_DEFAULT;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = 0xC8;
    param.announce_interval_max = 0xC8;
    param.conn_interval_min = PANEL_SLE_ONLINE_CONN_INTERVAL;
    param.conn_interval_max = PANEL_SLE_ONLINE_CONN_INTERVAL;
    param.conn_max_latency = 0;
    param.conn_supervision_timeout = 0x1F4;
    param.announce_tx_power = 20;
    param.own_addr = addr;
    ret = sle_set_announce_param(SLE_ADV_HANDLE_DEFAULT, &param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] set adv param fail: 0x%x\r\n", ret);
        return;
    }

    sle_announce_data_t data = {0};
    data.announce_data = g_announce_data;
    data.announce_data_len = sizeof(g_announce_data);
    data.seek_rsp_data = g_scan_rsp_data;
    data.seek_rsp_data_len = sizeof(g_scan_rsp_data);
    ret = sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] set adv data fail: 0x%x\r\n", ret);
        return;
    }

    ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] start announce fail: 0x%x\r\n", ret);
        return;
    }

    osal_printk("[PANEL_SLE_CLI] rx client gated until standalone session\r\n");
    start_status_listen_task_once();
    start_seek_if_needed();
}

static void sle_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t seek_cbks = {0};
    seek_cbks.announce_enable_cb = sle_announce_enable_cbk;
    seek_cbks.announce_disable_cb = sle_announce_disable_cbk;
    seek_cbks.announce_terminal_cb = sle_announce_terminal_cbk;
    seek_cbks.sle_enable_cb = sle_enable_cbk;
    seek_cbks.seek_enable_cb = sle_seek_enable_cbk;
    seek_cbks.seek_disable_cb = sle_seek_disable_cbk;
    seek_cbks.seek_result_cb = sle_seek_result_cbk;
    sle_announce_seek_register_callbacks(&seek_cbks);
}

static void ssapc_exchange_info_cbk(uint8_t client_id, uint16_t conn_id,
    ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    unused(param);
    unused(status);
    if (conn_id != g_rx_conn_id) {
        return;
    }

    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    (void)ssapc_find_structure(PANEL_SLE_CLIENT_ID, conn_id, &find_param);
}

static void ssapc_find_structure_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_service_result_t *service, errcode_t status)
{
    unused(client_id);
    if (conn_id != g_rx_conn_id || status != ERRCODE_SLE_SUCCESS ||
        service == NULL || !uuid16_equals(&service->uuid, SLE_JOB_SERVICE_UUID)) {
        return;
    }

    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PROPERTY;
    find_param.start_hdl = service->start_hdl;
    find_param.end_hdl = service->end_hdl;
    (void)ssapc_find_structure(PANEL_SLE_CLIENT_ID, conn_id, &find_param);
}

static void ssapc_find_property_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_property_result_t *property, errcode_t status)
{
    unused(client_id);
    if (conn_id != g_rx_conn_id || status != ERRCODE_SLE_SUCCESS || property == NULL) {
        return;
    }

    if (uuid16_equals(&property->uuid, SLE_JOB_DATA_CHAR_UUID)) {
        g_rx_data_handle = property->handle;
    } else if (uuid16_equals(&property->uuid, SLE_JOB_RESP_CHAR_UUID)) {
        g_rx_resp_handle = property->handle;
    }
    g_rx_handles_ready = (g_rx_data_handle != 0U) && (g_rx_resp_handle != 0U);
    if (g_rx_handles_ready) {
        osal_printk("[PANEL_SLE_CLI] rx ready conn=%u data=0x%x resp=0x%x\r\n",
                    (unsigned int)g_rx_conn_id, (unsigned int)g_rx_data_handle,
                    (unsigned int)g_rx_resp_handle);
        panel_model_set_transport_links(panel_transport_sle_tx_is_connected(), true);
    }
}

static void ssapc_write_cfm_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_write_result_t *write_result, errcode_t status)
{
    unused(client_id);
    unused(write_result);
    if (conn_id != g_rx_conn_id) {
        return;
    }
    g_last_write_cfm_status = status;
    if (g_write_cfm_sem_ready) {
        osal_sem_up(&g_write_cfm_sem);
    }
}

static void handle_rx_response_packet(const uint8_t *data, uint16_t len)
{
    sle_packet_view_t pkt;
    if (!sle_packet_decode(data, len, &pkt)) {
        osal_printk("[PANEL_SLE_CLI] bad rx response len=%u\r\n", len);
        return;
    }

    if (pkt.type == PKT_STATUS_RESP && pkt.len == sizeof(status_resp_payload_t)) {
        status_resp_payload_t st;
        (void)memcpy_s(&st, sizeof(st), pkt.payload, sizeof(st));
        apply_status_resp_as(&st, PANEL_OWNER_SCREEN, PANEL_MODE_OFFLINE);
    } else if (pkt.type == PKT_PANEL_STATUS && pkt.len == sizeof(panel_status_payload_t)) {
        panel_status_payload_t st;
        (void)memcpy_s(&st, sizeof(st), pkt.payload, sizeof(st));
        apply_panel_status(&st);
    }

    if (g_offline_response_cb != NULL) {
        g_offline_response_cb(data, len);
    }
    if (g_cmd_response_cb != NULL) {
        g_cmd_response_cb(data, len);
    }
}

static void ssapc_notification_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    if (conn_id != g_rx_conn_id || status != ERRCODE_SLE_SUCCESS ||
        data == NULL || data->data == NULL || data->handle != g_rx_resp_handle) {
        return;
    }
    handle_rx_response_packet(data->data, data->data_len);
}

static void ssapc_indication_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    ssapc_notification_cbk(client_id, conn_id, data, status);
}

static void sle_ssapc_register_cbks(void)
{
    ssapc_callbacks_t cbk = {0};
    cbk.exchange_info_cb = ssapc_exchange_info_cbk;
    cbk.find_structure_cb = ssapc_find_structure_cbk;
    cbk.ssapc_find_property_cbk = ssapc_find_property_cbk;
    cbk.write_cfm_cb = ssapc_write_cfm_cbk;
    cbk.notification_cb = ssapc_notification_cbk;
    cbk.indication_cb = ssapc_indication_cbk;
    ssapc_register_callbacks(&cbk);
}

errcode_t panel_transport_sle_start(void)
{
    g_panel_conn_id = PANEL_SLE_CONN_INVALID;
    g_rx_conn_id = PANEL_SLE_CONN_INVALID;
    g_service_handle = 0;
    g_status_property_handle = 0;
    g_rx_data_handle = 0;
    g_rx_resp_handle = 0;
    g_sle_enabled = false;
    g_seek_active = false;
    g_client_connecting = false;
    g_seek_for_status_listen = false;
    g_rx_handles_ready = false;
    memset(&g_rx_addr, 0, sizeof(g_rx_addr));
    g_rx_addr_valid = false;
    g_rx_disconnect_expected = false;
    g_rx_connect_cancel_after_connect = false;
    g_last_seek_ms = 0;
    g_status_listen_last_ms = 0;
    g_status_listen_started_ms = 0;
    g_last_write_cfm_status = ERRCODE_SLE_FAIL;
    g_standalone_session_active = false;
    if (!g_write_cfm_sem_ready && osal_sem_init(&g_write_cfm_sem, 0) == OSAL_SUCCESS) {
        g_write_cfm_sem_ready = true;
    }
    if (!g_write_mutex_ready && osal_mutex_init(&g_write_mutex) == OSAL_SUCCESS) {
        g_write_mutex_ready = true;
    }

    sle_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();
    sle_ssapc_register_cbks();

    errcode_t ret = enable_sle();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_SRV] enable_sle fail: 0x%x\r\n", ret);
    }
    return ret;
}

void panel_transport_sle_poll(void)
{
    stop_status_listen_if_expired();
    start_seek_if_needed();
    start_status_listen_if_needed();
}

bool panel_transport_sle_rx_is_connected(void)
{
    return g_rx_conn_id != PANEL_SLE_CONN_INVALID && g_rx_handles_ready;
}

bool panel_transport_sle_tx_is_connected(void)
{
    return g_panel_conn_id != PANEL_SLE_CONN_INVALID;
}

bool panel_transport_sle_can_control_rx(void)
{
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        return false;
    }
    return !panel_transport_sle_tx_is_connected() || g_standalone_session_active;
}

void panel_transport_sle_set_standalone_session_active(bool active)
{
    bool was_active = g_standalone_session_active;
    g_standalone_session_active = active;
    if (active) {
        g_rx_connect_cancel_after_connect = false;
    }
    if (!active) {
        if (g_client_connecting) {
            g_rx_connect_cancel_after_connect = true;
        }
    }
    if (!active && was_active && g_rx_conn_id != PANEL_SLE_CONN_INVALID) {
        disconnect_rx_expected();
    }
    start_seek_if_needed();
}

errcode_t panel_transport_sle_send_rx_packet(const void *data, uint16_t len)
{
    if (data == NULL || len == 0 || !panel_transport_sle_can_control_rx() ||
        !panel_transport_sle_rx_is_connected() || g_rx_data_handle == 0 ||
        !g_write_cfm_sem_ready || !g_write_mutex_ready) {
        return ERRCODE_SLE_FAIL;
    }

    osal_mutex_lock(&g_write_mutex);
    while (osal_sem_down_timeout(&g_write_cfm_sem, 0) == OSAL_SUCCESS) {
    }

    ssapc_write_param_t param = {0};
    param.handle = g_rx_data_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = len;
    param.data = (uint8_t *)data;

    uint8_t pkt_type = packet_type_from_encoded(data, len);
    if (pkt_type == PKT_JOB_DATA || pkt_type == PKT_STATUS_REQ) {
        errcode_t ret = ssapc_write_cmd(PANEL_SLE_CLIENT_ID, g_rx_conn_id, &param);
        if (ret != ERRCODE_SLE_SUCCESS) {
            osal_printk("[PANEL_SLE_CLI] write cmd fail type=0x%02x ret=0x%x len=%u\r\n",
                        pkt_type, ret, (unsigned int)len);
        }
        osal_mutex_unlock(&g_write_mutex);
        return ret;
    }

    g_last_write_cfm_status = ERRCODE_SLE_FAIL;
    errcode_t ret = ssapc_write_req(PANEL_SLE_CLIENT_ID, g_rx_conn_id, &param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[PANEL_SLE_CLI] write submit fail ret=0x%x len=%u\r\n",
                    ret, (unsigned int)len);
        osal_mutex_unlock(&g_write_mutex);
        return ret;
    }
    if (osal_sem_down_timeout(&g_write_cfm_sem, PANEL_SLE_WRITE_CFM_TIMEOUT_MS) != OSAL_SUCCESS) {
        osal_printk("[PANEL_SLE_CLI] write cfm timeout len=%u\r\n", (unsigned int)len);
        osal_mutex_unlock(&g_write_mutex);
        return ERRCODE_SLE_TIMEOUT;
    }
    ret = g_last_write_cfm_status;
    osal_mutex_unlock(&g_write_mutex);
    return ret;
}

void panel_transport_sle_set_rx_response_cb(panel_transport_rx_response_cb_t cb)
{
    g_offline_response_cb = cb;
}

void panel_transport_sle_set_cmd_response_cb(panel_transport_rx_response_cb_t cb)
{
    g_cmd_response_cb = cb;
}
