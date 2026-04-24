/**
 * @file safety_sle_server.c
 * @brief 安全终端节点 SLE Server
 */
#include "safety_sle_server.h"

#include "config.h"
#include "crc16.h"
#include "safety_protocol.h"

#include "common_def.h"
#include "securec.h"
#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_server.h"
#include "soc_osal.h"
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

static const uint8_t g_safety_mac[SLE_ADDR_LEN] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x04};

static uint16_t g_conn_hdl = 0;
static uint8_t g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_cmd_property_handle = 0;
static uint16_t g_status_property_handle = 0;
static uint16_t g_last_ack_seq = 0;

#define UUID_LEN_2 2

static uint8_t sle_uuid_base[] = {0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
                                  0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void sle_uuid_set_base(sle_uuid_t *out)
{
    (void)memcpy_s(out->uuid, SLE_UUID_LEN, sle_uuid_base, SLE_UUID_LEN);
    out->len = UUID_LEN_2;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    sle_uuid_set_base(out);
    out->len = UUID_LEN_2;
    out->uuid[14] = (uint8_t)(u2 & 0xFF);
    out->uuid[15] = (uint8_t)((u2 >> 8) & 0xFF);
}

static uint8_t safety_state_to_flags(const safety_service_state_t *state)
{
    uint8_t flags = 0;

    if (state->led_ready) {
        flags |= SAFETY_FLAG_LED_READY;
    }
    if (state->led_on) {
        flags |= SAFETY_FLAG_LED_ON;
    }
    return flags;
}

static void safety_build_status_packet(
    const safety_service_state_t *state, uint16_t ack_seq, uint8_t override_error, safety_node_status_t *packet)
{
    packet->version = SAFETY_PROTOCOL_VERSION;
    packet->status = state->status_code;
    packet->error_code = (override_error != SAFETY_ERR_NONE) ? override_error : state->error_code;
    packet->flags = safety_state_to_flags(state);
    packet->ack_seq = ack_seq;
    safety_status_set_crc(packet);
}

static errcode_t safety_send_status_packet(const safety_node_status_t *packet)
{
    ssaps_ntf_ind_t param = {0};

    if ((g_conn_hdl == 0U) || (g_status_property_handle == 0U) || (packet == NULL)) {
        return ERRCODE_FAIL;
    }

    param.handle = g_status_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value = (uint8_t *)packet;
    param.value_len = sizeof(*packet);
    return ssaps_notify_indicate(g_server_id, g_conn_hdl, &param);
}

static errcode_t safety_publish_current_state(uint8_t override_error)
{
    safety_service_state_t state = {0};
    safety_node_status_t packet = {0};
    errcode_t ret = safety_service_get_state(&state);

    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    safety_build_status_packet(&state, g_last_ack_seq, override_error, &packet);
    return safety_send_status_packet(&packet);
}

static errcode_t safety_execute_remote_cmd(const safety_node_cmd_t *cmd, uint8_t *override_error)
{
    *override_error = SAFETY_ERR_NONE;

    switch (cmd->cmd) {
        case SAFETY_CMD_LED_OFF:
            return safety_service_set_led(false);

        case SAFETY_CMD_LED_ON:
            return safety_service_set_led(true);

        default:
            *override_error = SAFETY_ERR_NOT_SUPPORTED;
            return ERRCODE_NOT_SUPPORT;
    }
}

static void ssaps_read_request_cbk(
    uint8_t server_id, uint16_t conn_id, ssaps_req_read_cb_t *read_cb_para, errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(read_cb_para);
    unused(status);
}

static void ssaps_write_request_cbk(
    uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    safety_node_cmd_t cmd = {0};
    uint8_t override_error = SAFETY_ERR_NONE;
    errcode_t ret;

    unused(server_id);
    unused(conn_id);
    unused(status);

    if ((write_cb_para == NULL) || (write_cb_para->value == NULL)) {
        return;
    }

    if (write_cb_para->length != sizeof(safety_node_cmd_t)) {
        osal_printk("[safety node] invalid cmd size: %u\r\n", write_cb_para->length);
        return;
    }

    memcpy(&cmd, write_cb_para->value, sizeof(cmd));
    if (!safety_cmd_check_crc(&cmd)) {
        osal_printk("[safety node] cmd crc error, seq=%u\r\n", cmd.seq);
        return;
    }

    g_last_ack_seq = cmd.seq;
    ret = safety_execute_remote_cmd(&cmd, &override_error);
    if ((ret != ERRCODE_SUCC) && (override_error == SAFETY_ERR_NONE)) {
        override_error = SAFETY_ERR_LED_IO;
    }

    if (safety_publish_current_state(override_error) != ERRCODE_SUCC) {
        osal_printk("[safety node] publish status after cmd fail: seq=%u ret=0x%x\r\n", cmd.seq, ret);
    }
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id, ssap_exchange_info_t *mtu_size, errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(status);
    osal_printk("[safety node] MTU changed: %d\r\n", mtu_size->mtu_size);
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(handle);
    unused(status);
    osal_printk("[safety node] service started\r\n");
}

static void safety_ssaps_register_cbks(void)
{
    ssaps_callbacks_t cbk = {0};
    cbk.start_service_cb = ssaps_start_service_cbk;
    cbk.mtu_changed_cb = ssaps_mtu_changed_cbk;
    cbk.read_request_cb = ssaps_read_request_cbk;
    cbk.write_request_cb = ssaps_write_request_cbk;
    ssaps_register_callbacks(&cbk);
}

static errcode_t safety_add_service(void)
{
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_SAFETY_SERVICE_UUID, &service_uuid);
    return ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
}

static errcode_t safety_add_cmd_property(void)
{
    ssaps_property_info_t property = {0};
    uint8_t init_val[sizeof(safety_node_cmd_t)] = {0};

    property.permissions = SLE_SAFETY_PROPERTIES;
    sle_uuid_setu2(SLE_SAFETY_CMD_CHAR_UUID, &property.uuid);
    property.value = init_val;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_WRITE | SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP;
    return ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_cmd_property_handle);
}

static errcode_t safety_add_status_property(void)
{
    ssaps_property_info_t property = {0};
    uint8_t init_val[sizeof(safety_node_status_t)] = {0};
    uint8_t ntf_value[] = {0x01, 0x00};

    property.permissions = SLE_SAFETY_PROPERTIES;
    sle_uuid_setu2(SLE_SAFETY_STATUS_CHAR_UUID, &property.uuid);
    property.value = init_val;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY;

    errcode_t ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_status_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    ssaps_desc_info_t desc = {0};
    desc.permissions = SLE_SAFETY_DESCRIPTOR;
    desc.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    desc.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    desc.value = ntf_value;
    desc.value_len = sizeof(ntf_value);
    return ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_status_property_handle, &desc);
}

static errcode_t safety_server_add(void)
{
    sle_uuid_t app_uuid = {0};
    char app_uuid_data[] = {0x0, 0x3};
    errcode_t ret;

    app_uuid.len = sizeof(app_uuid_data);
    memcpy_s(app_uuid.uuid, app_uuid.len, app_uuid_data, sizeof(app_uuid_data));
    ret = ssaps_register_server(&app_uuid, &g_server_id);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] register server fail: 0x%x\r\n", ret);
        return ret;
    }

    if (safety_add_service() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] add service fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (safety_add_cmd_property() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] add cmd property fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (safety_add_status_property() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] add status property fail\r\n");
        return ERRCODE_SLE_FAIL;
    }

    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] start service fail: 0x%x\r\n", ret);
        return ret;
    }
    return ERRCODE_SLE_SUCCESS;
}

static void safety_connect_state_changed_cbk(
    uint16_t conn_id, const sle_addr_t *addr, sle_acb_state_t conn_state, sle_pair_state_t pair_state,
    sle_disc_reason_t disc_reason)
{
    unused(addr);
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_hdl = conn_id;
        g_last_ack_seq = 0;
        osal_printk("[safety node] connected, conn_id=%u\r\n", conn_id);
        return;
    }

    if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_conn_hdl = 0;
        g_last_ack_seq = 0;
        osal_printk("[safety node] disconnected, restart advertise\r\n");
        sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    }
}

static void safety_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    osal_printk("[safety node] pair complete: 0x%x\r\n", status);
}

static void safety_auth_complete_cbk(
    uint16_t conn_id, const sle_addr_t *addr, errcode_t status, const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    osal_printk("[safety node] auth complete: 0x%x\r\n", status);
}

static void safety_conn_register_cbks(void)
{
    sle_connection_callbacks_t cbk = {0};
    cbk.connect_state_changed_cb = safety_connect_state_changed_cbk;
    cbk.pair_complete_cb = safety_pair_complete_cbk;
    cbk.auth_complete_cb = safety_auth_complete_cbk;
    sle_connection_register_callbacks(&cbk);
}

#define SLE_SAFETY_LOCAL_NAME_LEN 7
static uint8_t g_announce_data[] = {
    SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
    1,
    SLE_ANNOUNCE_LEVEL_NORMAL,
    SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS,
    2,
    (uint8_t)(SLE_SAFETY_SERVICE_UUID & 0xFF),
    (uint8_t)((SLE_SAFETY_SERVICE_UUID >> 8) & 0xFF),
};

static uint8_t g_scan_rsp_data[] = {SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
                                    1,
                                    10,
                                    SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME,
                                    SLE_SAFETY_LOCAL_NAME_LEN,
                                    'S',
                                    'a',
                                    'f',
                                    'e',
                                    'L',
                                    'E',
                                    'D'};

static errcode_t safety_adv_init(void)
{
    sle_announce_param_t param = {0};
    sle_announce_data_t data = {0};
    errcode_t ret;

    param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
    param.announce_handle = SLE_ADV_HANDLE_DEFAULT;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = 0xC8;
    param.announce_interval_max = 0xC8;
    param.conn_interval_min = 0x14;
    param.conn_interval_max = 0x14;
    param.conn_max_latency = 0;
    param.conn_supervision_timeout = 0x1F4;
    param.announce_tx_power = 10;
    param.own_addr.type = 0;
    memcpy_s(param.own_addr.addr, SLE_ADDR_LEN, g_safety_mac, SLE_ADDR_LEN);
    ret = sle_set_announce_param(param.announce_handle, &param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] set adv param fail: 0x%x\r\n", ret);
        return ret;
    }

    data.announce_data = g_announce_data;
    data.announce_data_len = sizeof(g_announce_data);
    data.seek_rsp_data = g_scan_rsp_data;
    data.seek_rsp_data_len = sizeof(g_scan_rsp_data);
    ret = sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] set adv data fail: 0x%x\r\n", ret);
        return ret;
    }

    return sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
}

static void safety_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("[safety node] adv enable id=%u status=0x%x\r\n", announce_id, status);
}

static void safety_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    unused(status);
}

static void safety_announce_terminal_cbk(uint32_t announce_id)
{
    unused(announce_id);
}

static void safety_enable_cbk(errcode_t status)
{
    errcode_t ret;
    sle_addr_t addr = {0};
    ssap_exchange_info_t info = {0};

    osal_printk("[safety node] sle enable: 0x%x\r\n", status);
    if (status != ERRCODE_SLE_SUCCESS) {
        return;
    }

    ret = safety_server_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] server add failed: 0x%x\r\n", ret);
        return;
    }

    info.mtu_size = 512;
    info.version = 1;
    ret = ssaps_set_info(g_server_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] set ssap info fail: 0x%x\r\n", ret);
        return;
    }

    addr.type = 0;
    memcpy_s(addr.addr, SLE_ADDR_LEN, g_safety_mac, SLE_ADDR_LEN);
    ret = sle_set_local_addr(&addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] set local addr fail: 0x%x\r\n", ret);
        return;
    }

    ret = safety_adv_init();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[safety node] adv init fail: 0x%x\r\n", ret);
        return;
    }
    osal_printk("[safety node] server init OK\r\n");
}

static void safety_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t cbk = {0};
    cbk.announce_enable_cb = safety_announce_enable_cbk;
    cbk.announce_disable_cb = safety_announce_disable_cbk;
    cbk.announce_terminal_cb = safety_announce_terminal_cbk;
    cbk.sle_enable_cb = safety_enable_cbk;
    sle_announce_seek_register_callbacks(&cbk);
}

errcode_t safety_sle_server_init(void)
{
    safety_announce_register_cbks();
    safety_conn_register_cbks();
    safety_ssaps_register_cbks();
    osal_printk("[safety node] enable_sle called\r\n");
    return enable_sle();
}

errcode_t safety_sle_server_publish_state(const safety_service_state_t *state)
{
    safety_node_status_t packet = {0};

    if ((state == NULL) || (g_conn_hdl == 0U)) {
        return ERRCODE_FAIL;
    }

    safety_build_status_packet(state, g_last_ack_seq, SAFETY_ERR_NONE, &packet);
    return safety_send_status_packet(&packet);
}

uint16_t safety_sle_server_get_conn_id(void)
{
    return g_conn_hdl;
}
