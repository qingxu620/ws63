/**
 * @file sle_job_server.c
 * @brief SLE server for structured laser job packets.
 */
#include "sle_job_route_server.h"
#include "common_def.h"
#include "sle_job_config.h"
#include "errcode.h"
#include "sle_job_protocol.h"
#include "securec.h"
#include "soc_osal.h"

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
#define SLE_CONN_BROADCAST 0xFFFF
#define SLE_JOB_ROUTE_MAX_CONNECTIONS 4U

static uint8_t g_receiver_mac[SLE_ADDR_LEN] = {0x20, 0x06, 0x09, 0x27, 0x00, 0x01};
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
    if (status != ERRCODE_SLE_SUCCESS || write_cb_para == NULL ||
        write_cb_para->value == NULL || write_cb_para->length == 0) {
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
        g_packet_cb(conn_id, write_cb_para->value, write_cb_para->length);
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

static void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(addr);
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        conn_table_add(conn_id);
        errcode_t adv_ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
        unused(adv_ret);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        bool was_owner = (conn_id == g_owner_conn_id);
        (void)conn_table_remove(conn_id);
        if (was_owner) {
            g_owner_conn_id = SLE_CONN_INVALID;
        }
        if (g_server_stopping) {
            return;
        }
        if (was_owner && g_disconnect_cb != NULL) {
            osal_printk("[job_rx] owner disconnected, force safe stop\r\n");
            g_disconnect_cb();
        }
        sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
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

static uint8_t g_announce_data[] = {
    SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
    1,
    SLE_ANNOUNCE_LEVEL_NORMAL,
    SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS,
    2,
    (uint8_t)(SLE_JOB_SERVICE_UUID & 0xFF),
    (uint8_t)(SLE_JOB_SERVICE_UUID >> 8),
};

static uint8_t g_scan_rsp_data[] = {
    SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
    1,
    20,
    SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME,
    sizeof(SLE_JOB_RECEIVER_NAME) - 1,
    's', 'l', 'e', '_', 'j', 'o', 'b', '_', 'r', 'x'
};

static void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] adv enable fail status=0x%02x\r\n", status);
    }
}

static void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] adv disable fail status=0x%02x\r\n", status);
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

    sle_announce_data_t data = {0};
    data.announce_data = g_announce_data;
    data.announce_data_len = sizeof(g_announce_data);
    data.seek_rsp_data = g_scan_rsp_data;
    data.seek_rsp_data_len = sizeof(g_scan_rsp_data);
    ret = sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] set adv data fail: 0x%x\r\n", ret);
        return;
    }

    ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] start announce fail: 0x%x\r\n", ret);
        return;
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

errcode_t sle_job_route_server_init(void)
{
    conn_table_reset();
    g_server_stopping = false;
    sle_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();

    errcode_t ret = enable_sle();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[job_rx] enable_sle fail: 0x%x\r\n", ret);
    }
    return ret;
}

errcode_t sle_job_route_server_stop(void)
{
    g_server_stopping = true;
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

    ssaps_ntf_ind_t param = {0};
    param.handle = g_resp_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value_len = len;
    param.value = (uint8_t *)data;
    return ssaps_notify_indicate(g_server_id, g_owner_conn_id, &param);
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
