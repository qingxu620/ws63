/**
 * @file panel_transport_sle.c
 * @brief SLE server used by the screen panel to receive TX-mirrored status.
 */
#include "panel_transport_sle.h"
#include "panel_model.h"

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

#define PANEL_SLE_CONN_INVALID 0xFFFF

static uint8_t g_panel_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x02};
static uint8_t g_server_id = 0;
static uint16_t g_conn_id = PANEL_SLE_CONN_INVALID;
static uint16_t g_service_handle = 0;
static uint16_t g_status_property_handle = 0;
static uint32_t g_rx_panel_status_count = 0;

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

static uint8_t panel_mode_from_job_state(uint8_t job_state, uint8_t status)
{
    if (status != JOB_STATUS_OK || job_state == JOB_STATE_ERROR) {
        return PANEL_MODE_ERROR;
    }
    return PANEL_MODE_ONLINE;
}

static void apply_status_resp(const status_resp_payload_t *st)
{
    if (st == NULL) {
        return;
    }

    uint8_t flags = PANEL_STATUS_FLAG_OWNER_LINK | PANEL_STATUS_FLAG_ANY_LINK;
    uint8_t mode = panel_mode_from_job_state(st->state, st->status);
    panel_model_apply_rx_panel_status(PANEL_OWNER_HOST, mode, st->state, flags,
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
    panel_model_apply_rx_panel_status(st->owner, st->mode, st->job_state, st->flags, st->seq,
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
        g_rx_panel_status_count++;
        if ((g_rx_panel_status_count & 0x07U) == 1U) {
            osal_printk("[PANEL_SLE_SRV] panel_status seq=%u owner=%u mode=%u state=%u flags=0x%02x rx=%u/%u\r\n",
                        (unsigned int)st.seq, st.owner, st.mode, st.job_state, st.flags,
                        (unsigned int)st.received_size, (unsigned int)st.total_size);
        }
        return;
    }

    if (pkt.type == PKT_STATUS_RESP && pkt.len == sizeof(status_resp_payload_t)) {
        status_resp_payload_t st;
        (void)memcpy_s(&st, sizeof(st), pkt.payload, sizeof(st));
        apply_status_resp(&st);
        g_rx_panel_status_count++;
        if ((g_rx_panel_status_count & 0x07U) == 1U) {
            osal_printk("[PANEL_SLE_SRV] status_resp state=%u status=%u job=%u rx=%u/%u\r\n",
                        st.state, st.status, (unsigned int)st.job_id,
                        (unsigned int)st.received_size, (unsigned int)st.total_size);
        }
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

    g_conn_id = conn_id;
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
    unused(status);
    if (mtu_size != NULL) {
        osal_printk("[PANEL_SLE_SRV] MTU changed: %u\r\n", mtu_size->mtu_size);
    }
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(handle);
    osal_printk("[PANEL_SLE_SRV] service start status=0x%x\r\n", status);
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
    unused(addr);
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_id = conn_id;
        osal_printk("[PANEL_SLE_SRV] connected conn_id=%u\r\n", conn_id);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED && conn_id == g_conn_id) {
        g_conn_id = PANEL_SLE_CONN_INVALID;
        osal_printk("[PANEL_SLE_SRV] disconnected\r\n");
        panel_model_apply_rx_panel_status(PANEL_OWNER_NONE, PANEL_MODE_LINK_LOST, JOB_STATE_IDLE,
                                          0, g_model.seq + 1U, 0, 0, 0, 0, 0, 1, 0U);
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    osal_printk("[PANEL_SLE_SRV] pair complete: 0x%x\r\n", status);
}

static void sle_auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr,
    errcode_t status, const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    osal_printk("[PANEL_SLE_SRV] auth complete: 0x%x\r\n", status);
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
    osal_printk("[PANEL_SLE_SRV] adv enable id=0x%02x status=0x%02x\r\n", announce_id, status);
}

static void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("[PANEL_SLE_SRV] adv disable id=0x%02x status=0x%02x\r\n", announce_id, status);
}

static void sle_announce_terminal_cbk(uint32_t announce_id)
{
    osal_printk("[PANEL_SLE_SRV] adv terminal id=0x%02x\r\n", announce_id);
}

static void sle_enable_cbk(errcode_t status)
{
    osal_printk("[PANEL_SLE_SRV] enable status=0x%02x\r\n", status);
    if (status != ERRCODE_SLE_SUCCESS) {
        return;
    }

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
    param.conn_interval_min = 0x14;
    param.conn_interval_max = 0x14;
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
    osal_printk("[PANEL_SLE_SRV] advertising as '%s'\r\n", SLE_PANEL_SERVER_NAME);
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

errcode_t panel_transport_sle_start(void)
{
    g_conn_id = PANEL_SLE_CONN_INVALID;
    g_service_handle = 0;
    g_status_property_handle = 0;
    g_rx_panel_status_count = 0;

    sle_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();

    errcode_t ret = enable_sle();
    osal_printk("[PANEL_SLE_SRV] enable_sle ret=0x%x\r\n", ret);
    return ret;
}
