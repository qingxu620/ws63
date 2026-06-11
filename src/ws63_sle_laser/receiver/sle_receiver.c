/**
 * @file sle_receiver.c
 * @brief SLE receiver - receives G-code and sends response back.
 *
 * Design:
 *   - Advertise as "sle_laser_rx"
 *   - Receive G-code strings from transmitter
 *   - Send response (ok/error/status) back to transmitter
 */
#include "sle_receiver.h"
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
#include "sle_ssap_server.h"

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

#define UUID_LEN_2 2
#define SLE_CONN_INVALID 0xFFFF
#define SLE_GCODE_LINE_MAX 128

/* State */
static uint16_t g_conn_id = SLE_CONN_INVALID;
static uint8_t g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_data_property_handle = 0;  /* For receiving G-code */
static uint16_t g_resp_property_handle = 0;  /* For sending response */

/* UUID base */
static uint8_t sle_uuid_base[] = {
    0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* External callback from main.c to process G-code line */
extern void sle_gcode_line_received(const char *line, uint16_t len);

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

/* SSAP Server callbacks */
static void ssaps_write_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    unused(conn_id);

    if (status != ERRCODE_SLE_SUCCESS || write_cb_para == NULL ||
        write_cb_para->value == NULL || write_cb_para->length == 0) {
        return;
    }

    /* Received G-code from transmitter - feed to processor */
    char line[SLE_GCODE_LINE_MAX];
    uint16_t len = write_cb_para->length;
    if (len >= SLE_GCODE_LINE_MAX) {
        len = SLE_GCODE_LINE_MAX - 1;
    }
    memcpy_s(line, sizeof(line), write_cb_para->value, len);
    line[len] = '\0';

    osal_printk("[rx] sle rx: %s\r\n", line);
    sle_gcode_line_received(line, len);
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
    ssaps_callbacks_t ssaps_cbk = {0};
    ssaps_cbk.start_service_cb = ssaps_start_service_cbk;
    ssaps_cbk.mtu_changed_cb = ssaps_mtu_changed_cbk;
    ssaps_cbk.read_request_cb = ssaps_read_request_cbk;
    ssaps_cbk.write_request_cb = ssaps_write_request_cbk;
    ssaps_register_callbacks(&ssaps_cbk);
}

static errcode_t sle_uuid_server_service_add(void)
{
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_RECEIVER_SERVICE_UUID, &service_uuid);
    return ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
}

static errcode_t sle_uuid_server_data_property_add(void)
{
    ssaps_property_info_t property = {0};

    sle_uuid_setu2(SLE_RECEIVER_DATA_CHAR_UUID, &property.uuid);
    property.permissions = 0x01 | 0x02; /* Read | Write */
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    property.value = NULL;
    property.value_len = 0;

    return ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_data_property_handle);
}

static errcode_t sle_uuid_server_resp_property_add(void)
{
    ssaps_property_info_t property = {0};

    sle_uuid_setu2(SLE_RECEIVER_RESP_CHAR_UUID, &property.uuid);
    property.permissions = 0x01 | 0x02; /* Read | Write */
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY;
    property.value = NULL;
    property.value_len = 0;

    return ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_resp_property_handle);
}

static errcode_t sle_uuid_server_add(void)
{
    sle_uuid_t app_uuid = {0};
    app_uuid.len = UUID_LEN_2;

    ssaps_register_server(&app_uuid, &g_server_id);

    errcode_t ret = sle_uuid_server_service_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ret;
    }

    ret = sle_uuid_server_data_property_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ret;
    }

    ret = sle_uuid_server_resp_property_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ret;
    }

    osal_printk("[rx] server added id:0x%x service:0x%x data:0x%x resp:0x%x\r\n",
                g_server_id, g_service_handle, g_data_property_handle, g_resp_property_handle);

    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[rx] start service fail: 0x%x\r\n", ret);
        return ret;
    }

    ssap_exchange_info_t info = {0};
    info.mtu_size = 512;
    info.version = 1;
    ssaps_set_info(g_server_id, &info);

    return ERRCODE_SLE_SUCCESS;
}

/* Connection callbacks */
static void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(addr);
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_id = conn_id;
        osal_printk("[rx] connected! conn_id=%u\r\n", conn_id);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_conn_id = SLE_CONN_INVALID;
        osal_printk("[rx] disconnected\r\n");
        sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    osal_printk("[rx] pair complete: 0x%x\r\n", status);
}

static void sle_conn_register_cbks(void)
{
    sle_connection_callbacks_t conn_cbks = {0};
    conn_cbks.connect_state_changed_cb = sle_connect_state_changed_cbk;
    conn_cbks.pair_complete_cb = sle_pair_complete_cbk;
    sle_connection_register_callbacks(&conn_cbks);
}

/* Advertising callbacks */
static void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("[rx] adv enable id:0x%02x status:0x%02x\r\n", announce_id, status);
}

static void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("[rx] adv disable id:0x%02x status:0x%02x\r\n", announce_id, status);
}

static void sle_announce_terminal_cbk(uint32_t announce_id)
{
    osal_printk("[rx] adv terminal id:0x%02x\r\n", announce_id);
}

static void sle_enable_cbk(errcode_t status)
{
    osal_printk("[rx] enable status:0x%02x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        sle_uuid_server_add();

        /* Set local address */
        sle_addr_t addr = {0};
        uint8_t mac[SLE_ADDR_LEN] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
        addr.type = 0;
        memcpy_s(addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);
        sle_set_local_addr(&addr);

        /* Set advertising parameters */
        sle_announce_param_t param = {0};
        param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
        param.announce_handle = SLE_ADV_HANDLE_DEFAULT;
        param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
        param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
        param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
        param.announce_interval_min = 0xC8;
        param.announce_interval_max = 0xC8;
        param.conn_interval_min = 0x64;
        param.conn_interval_max = 0x64;
        param.conn_max_latency = 0x1F3;
        param.conn_supervision_timeout = 0x1F4;
        param.announce_tx_power = 20;
        param.own_addr = addr;
        sle_set_announce_param(SLE_ADV_HANDLE_DEFAULT, &param);

        /* Set advertising data */
        uint8_t adv_data[] = {
            SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL, 0x01, SLE_ANNOUNCE_LEVEL_NORMAL,
            SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS, 0x02,
            (uint8_t)(SLE_RECEIVER_SERVICE_UUID & 0xFF), (uint8_t)(SLE_RECEIVER_SERVICE_UUID >> 8)
        };

        uint8_t scan_rsp_data[32];
        uint8_t name_len = (uint8_t)strlen(SLE_RECEIVER_NAME);
        if (name_len > 28) name_len = 28;
        scan_rsp_data[0] = SLE_ADV_DATA_TYPE_TX_POWER_LEVEL;
        scan_rsp_data[1] = 0x01;
        scan_rsp_data[2] = 20;
        scan_rsp_data[3] = SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME;
        scan_rsp_data[4] = name_len;
        memcpy_s(&scan_rsp_data[5], sizeof(scan_rsp_data) - 5, SLE_RECEIVER_NAME, name_len);

        sle_announce_data_t data = {0};
        data.announce_data = adv_data;
        data.announce_data_len = sizeof(adv_data);
        data.seek_rsp_data = scan_rsp_data;
        data.seek_rsp_data_len = 5 + name_len;
        sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);

        sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
        osal_printk("[rx] advertising as '%s'\r\n", SLE_RECEIVER_NAME);
    }
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

/* Public API */
errcode_t sle_receiver_init(void)
{
    g_conn_id = SLE_CONN_INVALID;

    sle_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();

    errcode_t ret = enable_sle();
    osal_printk("[rx] enable_sle ret=0x%x\r\n", ret);
    return ret;
}

bool sle_receiver_is_connected(void)
{
    return g_conn_id != SLE_CONN_INVALID;
}

errcode_t sle_receiver_send_response(const char *data, uint16_t len)
{
    if (data == NULL || len == 0 || g_conn_id == SLE_CONN_INVALID || g_resp_property_handle == 0) {
        return ERRCODE_SLE_FAIL;
    }

    ssaps_ntf_ind_t param = {0};
    param.handle = g_resp_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value_len = len;
    param.value = (uint8_t *)data;

    return ssaps_notify_indicate(g_server_id, g_conn_id, &param);
}

const char *sle_receiver_get_status(void)
{
    return (g_conn_id != SLE_CONN_INVALID) ? "connected" : "advertising";
}
