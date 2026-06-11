/**
 * @file sle_passthrough.c
 * @brief Bidirectional SLE-UART bridge - wireless serial port.
 *
 * Design:
 *   UART RX → SLE TX (G-code to receiver)
 *   SLE RX → UART TX (ok/error/status from receiver)
 */
#include "sle_passthrough.h"
#include "common_def.h"
#include "errcode.h"
#include "securec.h"
#include "soc_osal.h"
#include "systick.h"

/* SLE headers */
#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"

#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME
#define SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME 0x0B
#endif

#ifndef SLE_SEEK_ACTIVE
#define SLE_SEEK_ACTIVE 0x01
#endif

#define SLE_CLIENT_ID 0
#define SLE_CONN_INVALID 0xFFFF
#define UUID_LEN_2 2

/* State */
static uint16_t g_conn_id = SLE_CONN_INVALID;
static bool g_seek_active = false;
static bool g_connect_pending = false;
static sle_addr_t g_pending_addr = {0};
static bool g_handles_ready = false;
static uint16_t g_data_handle = 0;    /* For sending G-code to receiver */
static uint16_t g_resp_handle = 0;    /* For receiving response from receiver */

/* Response callback */
static sle_response_cb_t g_response_cb = NULL;

/* UUID base */
static uint8_t sle_uuid_base[] = {
    0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

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

static bool uuid16_equals(const sle_uuid_t *uuid, uint16_t expect)
{
    return (uuid != NULL) && (uuid->len == UUID_LEN_2) &&
           (uuid->uuid[14] == (uint8_t)(expect & 0xFF)) &&
           (uuid->uuid[15] == (uint8_t)((expect >> 8) & 0xFF));
}

static bool seek_name_match(const sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL || seek_result->data == NULL) {
        return false;
    }

    uint8_t *data = seek_result->data;
    uint16_t len = seek_result->data_length;
    size_t name_len = strlen(SLE_RECEIVER_NAME);
    for (uint16_t i = 0; i < len;) {
        if ((i + 1U) >= len) break;
        uint8_t ad_type = data[i];
        uint8_t ad_len = data[i + 1U];
        if ((uint16_t)(i + 2U + ad_len) > len) break;
        if (ad_type == SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME && ad_len == name_len &&
            memcmp(&data[i + 2U], SLE_RECEIVER_NAME, name_len) == 0) {
            return true;
        }
        i += (uint16_t)ad_len + 2U;
    }
    return false;
}

static void start_seek_if_needed(void)
{
    if (g_seek_active || g_connect_pending || g_conn_id != SLE_CONN_INVALID) {
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
    sle_set_seek_param(&param);
    (void)sle_start_seek();
}

/* SLE callbacks */
static void sle_enable_cbk(errcode_t status)
{
    osal_printk("[tx] sle enable: 0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        start_seek_if_needed();
    }
}

static void seek_result_cbk(sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL || g_connect_pending || g_conn_id != SLE_CONN_INVALID) {
        return;
    }
    if (seek_name_match(seek_result)) {
        memcpy_s(&g_pending_addr, sizeof(g_pending_addr), &seek_result->addr, sizeof(seek_result->addr));
        g_connect_pending = true;
        osal_printk("[tx] found receiver, connecting...\r\n");
        (void)sle_stop_seek();
    }
}

static void seek_enable_cbk(errcode_t status)
{
    g_seek_active = (status == ERRCODE_SLE_SUCCESS);
}

static void seek_disable_cbk(errcode_t status)
{
    g_seek_active = false;
    if (status == ERRCODE_SLE_SUCCESS && g_connect_pending) {
        errcode_t ret = sle_connect_remote_device(&g_pending_addr);
        if (ret != ERRCODE_SLE_SUCCESS) {
            osal_printk("[tx] connect fail: 0x%x\r\n", ret);
            g_connect_pending = false;
            start_seek_if_needed();
        }
    }
}

static void connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(addr);
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_id = conn_id;
        g_connect_pending = false;
        osal_printk("[tx] connected! conn_id=%u\r\n", conn_id);
        /* Start service discovery */
        ssap_exchange_info_t info = {0};
        info.mtu_size = 512;
        info.version = 1;
        ssapc_exchange_info_req(SLE_CLIENT_ID, g_conn_id, &info);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_conn_id = SLE_CONN_INVALID;
        g_handles_ready = false;
        g_data_handle = 0;
        g_resp_handle = 0;
        osal_printk("[tx] disconnected\r\n");
        start_seek_if_needed();
    }
}

static void exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(param);
    osal_printk("[tx] mtu exchange: 0x%x\r\n", status);
    /* Discover services */
    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    (void)ssapc_find_structure(SLE_CLIENT_ID, g_conn_id, &find_param);
}

static void find_structure_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_service_result_t *service, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || service == NULL) return;
    if (!uuid16_equals(&service->uuid, SLE_PASSTHROUGH_SERVICE_UUID)) return;

    osal_printk("[tx] found service 0x%x\r\n", SLE_PASSTHROUGH_SERVICE_UUID);
    /* Find properties */
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
    unused(conn_id);
    if (status != ERRCODE_SLE_SUCCESS || property == NULL) return;

    if (uuid16_equals(&property->uuid, SLE_PASSTHROUGH_DATA_CHAR_UUID)) {
        g_data_handle = property->handle;
        osal_printk("[tx] data handle: 0x%x\r\n", g_data_handle);
    } else if (uuid16_equals(&property->uuid, SLE_PASSTHROUGH_RESP_CHAR_UUID)) {
        g_resp_handle = property->handle;
        osal_printk("[tx] resp handle: 0x%x\r\n", g_resp_handle);
    }

    g_handles_ready = (g_data_handle != 0) && (g_resp_handle != 0);
    if (g_handles_ready) {
        osal_printk("[tx] ready! bidirectional link established\r\n");
    }
}

static void write_cfm_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_write_result_t *write_result, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(write_result);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[tx] write cfm fail: 0x%x\r\n", status);
    }
}

/* Notification callback - receives response from receiver */
static void notification_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);

    if (status != ERRCODE_SLE_SUCCESS || data == NULL || data->data == NULL) {
        return;
    }

    /* Only process notifications from response handle */
    if (data->handle == g_resp_handle && g_response_cb != NULL) {
        g_response_cb(data->data, data->data_len);
    }
}

static void indication_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    notification_cbk(client_id, conn_id, data, status);
}

/* Public API */
errcode_t sle_passthrough_init(void)
{
    g_conn_id = SLE_CONN_INVALID;
    g_seek_active = false;
    g_connect_pending = false;
    g_handles_ready = false;
    g_data_handle = 0;
    g_resp_handle = 0;

    /* Register seek callbacks */
    sle_announce_seek_callbacks_t seek_cbk = {0};
    seek_cbk.sle_enable_cb = sle_enable_cbk;
    seek_cbk.seek_enable_cb = seek_enable_cbk;
    seek_cbk.seek_disable_cb = seek_disable_cbk;
    seek_cbk.seek_result_cb = seek_result_cbk;
    sle_announce_seek_register_callbacks(&seek_cbk);

    /* Register connection callbacks */
    sle_connection_callbacks_t conn_cbk = {0};
    conn_cbk.connect_state_changed_cb = connect_state_changed_cbk;
    sle_connection_register_callbacks(&conn_cbk);

    /* Register SSAPC callbacks */
    ssapc_callbacks_t ssapc_cbk = {0};
    ssapc_cbk.exchange_info_cb = exchange_info_cbk;
    ssapc_cbk.find_structure_cb = find_structure_cbk;
    ssapc_cbk.ssapc_find_property_cbk = find_property_cbk;
    ssapc_cbk.write_cfm_cb = write_cfm_cbk;
    ssapc_cbk.notification_cb = notification_cbk;
    ssapc_cbk.indication_cb = indication_cbk;
    ssapc_register_callbacks(&ssapc_cbk);

    /* Enable SLE */
    errcode_t ret = enable_sle();
    osal_printk("[tx] enable_sle ret=0x%x\r\n", ret);
    return ret;
}

errcode_t sle_passthrough_send_line(const char *line, uint16_t len)
{
    if (line == NULL || len == 0 || !g_handles_ready || g_data_handle == 0) {
        return ERRCODE_SLE_FAIL;
    }

    ssapc_write_param_t param = {0};
    param.handle = g_data_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = len;
    param.data = (uint8_t *)line;

    return ssapc_write_cmd(SLE_CLIENT_ID, g_conn_id, &param);
}

bool sle_passthrough_is_connected(void)
{
    return g_conn_id != SLE_CONN_INVALID && g_handles_ready;
}

const char *sle_passthrough_get_status(void)
{
    if (g_conn_id == SLE_CONN_INVALID) {
        return g_connect_pending ? "connecting" : "scanning";
    }
    return g_handles_ready ? "ready" : "discovering";
}

void sle_passthrough_set_response_cb(sle_response_cb_t cb)
{
    g_response_cb = cb;
}
