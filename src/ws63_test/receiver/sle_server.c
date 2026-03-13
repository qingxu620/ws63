/**
 * @file sle_server.c
 * @brief 接收板 SLE Server 实现
 *        基于 sle_speed_server 示例改造，接收 motion_cmd_t 数据包
 */
#include "sle_server.h"
#include "config.h"
#include "protocol.h"
#include "crc16.h"
#include "cmd_queue.h"
#include "safety_monitor.h"
#include "interpolator.h"
#include "laser_ctrl.h"

#include "app_init.h"
#include "securec.h"
#include "soc_osal.h"
#include "common_def.h"
#include "sle_common.h"
#include "sle_errcode.h"
#include "sle_ssap_server.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "systick.h"
#include <math.h>

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

/* ================= SLE 句柄 ================= */
static uint16_t g_conn_hdl = 0;
static uint8_t g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_cmd_property_handle = 0;    /* 接收命令的 property */
static uint16_t g_status_property_handle = 0; /* 发送状态的 property */
static uint16_t g_last_accepted_seq = 0;
static uint32_t g_heartbeat_rx_count = 0;
static uint64_t g_last_heartbeat_log_ms = 0;
static uint64_t g_last_status_report_ms = 0;

/* ================= UUID 设置 ================= */
#define UUID_LEN_2 2

static uint8_t sle_uuid_base[] = {0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
                                  0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#define MAX_COORD_MM ((float)DAC_MAX / (float)BEILV)
#define MIN_FEED_RATE_MM_MIN 0.1f
#define MAX_FEED_RATE_MM_MIN ((float)G0_FEED_RATE)

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

static void sle_send_status_pkt(uint8_t status, uint8_t error_code, uint16_t ack_seq)
{
    status_full_pkt_t full = {0};
    full.base.status = status;
    full.base.error_code = error_code;
    full.base.ack_seq = ack_seq;
    full.base.queue_free = cmd_queue_free_count();
    full.cur_x = (float)interpolator_get_x();
    full.cur_y = (float)interpolator_get_y();
    status_pkt_set_crc(&full.base);

    errcode_t ret = sle_laser_server_send_status((uint8_t *)&full, sizeof(full));
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] send status fail: 0x%x\r\n", ret);
        return;
    }
    g_last_status_report_ms = uapi_systick_get_ms();
}

#if (HEARTBEAT_STATUS_REPORT_INTERVAL_MS > 0)
static uint8_t sle_runtime_status(void)
{
    /*
     * 运行状态只反映“是否仍有运动在执行/排队”，不把激光使能本身等同于 Run。
     * 否则 M3 后即使位置已稳定，状态也会长期卡在 Run，误导上位机和自动测试。
     */
    if (interpolator_is_busy() || (cmd_queue_free_count() < (CMD_QUEUE_SIZE - 1))) {
        return STATUS_RUNNING;
    }
    return STATUS_IDLE;
}
#endif

static bool is_finite_in_range(float val, float min_val, float max_val)
{
    return isfinite(val) && (val >= min_val) && (val <= max_val);
}

/* 入队前参数校验:
 * - 未知命令直接拒收
 * - 运动指令要求坐标在机械范围内，速度为有限正值 */
static bool validate_motion_cmd(const motion_cmd_t *cmd, uint8_t *error_code)
{
    if (cmd == NULL || error_code == NULL) {
        return false;
    }

    *error_code = STATUS_ERR_NONE;

    switch (cmd->cmd) {
        case CMD_G0_MOVE:
        case CMD_G1_MOVE:
            if (!is_finite_in_range(cmd->target_x, 0.0f, MAX_COORD_MM) ||
                !is_finite_in_range(cmd->target_y, 0.0f, MAX_COORD_MM) ||
                !is_finite_in_range(cmd->feed_rate, MIN_FEED_RATE_MM_MIN, MAX_FEED_RATE_MM_MIN)) {
                *error_code = STATUS_ERR_INVALID_PARAM;
                return false;
            }
            return true;

        case CMD_LASER_ON:
            if (cmd->laser_pwr > (uint16_t)LASER_S_MAX) {
                *error_code = STATUS_ERR_INVALID_PARAM;
                return false;
            }
            return true;

        case CMD_LASER_OFF:
        case CMD_SET_ORIGIN:
        case CMD_SET_MODE:
        case CMD_HEARTBEAT:
            return true;

        default:
            *error_code = STATUS_ERR_INVALID_CMD;
            return false;
    }
}

/* ================= SSAP 回调 ================= */
static void ssaps_read_request_cbk(uint8_t server_id,
                                   uint16_t conn_id,
                                   ssaps_req_read_cb_t *read_cb_para,
                                   errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(read_cb_para);
    unused(status);
    osal_printk("[laser rx] read request\r\n");
}

/**
 * @brief 写请求回调 — 接收发射板发来的 motion_cmd_t
 *        这是整个接收板的数据入口
 */
static void ssaps_write_request_cbk(uint8_t server_id,
                                    uint16_t conn_id,
                                    ssaps_req_write_cb_t *write_cb_para,
                                    errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(status);

    if (write_cb_para == NULL || write_cb_para->value == NULL) {
        return;
    }

    /* 检查数据长度 */
    if (write_cb_para->length != sizeof(motion_cmd_t)) {
        osal_printk("[laser rx] invalid cmd size: %d\r\n", write_cb_para->length);
        return;
    }

    /* 解析命令 */
    motion_cmd_t cmd;
    memcpy(&cmd, write_cb_para->value, sizeof(motion_cmd_t));

    /* CRC 校验 */
    if (!motion_cmd_check_crc(&cmd)) {
        osal_printk("[laser rx] CRC error, seq=%u\r\n", cmd.seq);
        return;
    }

    uint8_t validate_err = STATUS_ERR_NONE;
    if (!validate_motion_cmd(&cmd, &validate_err)) {
        osal_printk("[laser rx] invalid cmd, type=0x%x seq=%u err=%u\r\n", cmd.cmd, cmd.seq, validate_err);
        sle_send_status_pkt(STATUS_ERROR, validate_err, g_last_accepted_seq);
        return;
    }

    /* 更新安全看门狗 */
    safety_update_last_sle_time();

    /* 心跳包不入队 */
    if (cmd.cmd == CMD_HEARTBEAT) {
        g_heartbeat_rx_count++;
        uint64_t now = uapi_systick_get_ms();
        if ((g_last_heartbeat_log_ms == 0) || ((now - g_last_heartbeat_log_ms) >= 1000)) {
            g_last_heartbeat_log_ms = now;
            osal_printk("[laser rx] heartbeat rx=%u, queue_free=%u\r\n", g_heartbeat_rx_count, cmd_queue_free_count());
        }
#if (HEARTBEAT_STATUS_REPORT_INTERVAL_MS > 0)
        if ((g_last_status_report_ms == 0) ||
            ((now - g_last_status_report_ms) >= HEARTBEAT_STATUS_REPORT_INTERVAL_MS)) {
            sle_send_status_pkt(sle_runtime_status(), STATUS_ERR_NONE, g_last_accepted_seq);
        }
#endif
        return;
    }

    /* 压入命令队列 */
    if (!cmd_queue_push(&cmd)) {
        osal_printk("[laser rx] queue full! drop seq=%u\r\n", cmd.seq);
        sle_send_status_pkt(STATUS_ERROR, STATUS_ERR_QUEUE_FULL, g_last_accepted_seq);
        return;
    }

    g_last_accepted_seq = cmd.seq;
    sle_send_status_pkt(STATUS_RUNNING, STATUS_ERR_NONE, g_last_accepted_seq);
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id, ssap_exchange_info_t *mtu_size, errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(status);
    osal_printk("[laser rx] MTU changed: %d\r\n", mtu_size->mtu_size);
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(handle);
    unused(status);
    osal_printk("[laser rx] service started\r\n");
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

/* ================= 服务注册 ================= */
static errcode_t sle_laser_add_service(void)
{
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_LASER_SERVICE_UUID, &service_uuid);
    return ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
}

static errcode_t sle_laser_add_cmd_property(void)
{
    ssaps_property_info_t property = {0};
    uint8_t init_val[sizeof(motion_cmd_t)] = {0};

    property.permissions = SLE_LASER_PROPERTIES;
    sle_uuid_setu2(SLE_LASER_CMD_CHAR_UUID, &property.uuid);
    property.value = init_val;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_WRITE | SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP;

    return ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_cmd_property_handle);
}

static errcode_t sle_laser_add_status_property(void)
{
    ssaps_property_info_t property = {0};
    uint8_t init_val[sizeof(status_full_pkt_t)] = {0};
    uint8_t ntf_value[] = {0x01, 0x00};

    property.permissions = SLE_LASER_PROPERTIES;
    sle_uuid_setu2(SLE_LASER_STATUS_CHAR_UUID, &property.uuid);
    property.value = init_val;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY;

    errcode_t ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_status_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        return ret;
    }

    /* 添加 CCCD 描述符 */
    ssaps_desc_info_t desc = {0};
    desc.permissions = SLE_LASER_DESCRIPTOR;
    desc.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    desc.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    desc.value = ntf_value;
    desc.value_len = sizeof(ntf_value);

    return ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_status_property_handle, &desc);
}

static errcode_t sle_laser_server_add(void)
{
    sle_uuid_t app_uuid = {0};
    char app_uuid_data[] = {0x0, 0x0};
    app_uuid.len = sizeof(app_uuid_data);
    memcpy_s(app_uuid.uuid, app_uuid.len, app_uuid_data, sizeof(app_uuid_data));
    errcode_t ret = ssaps_register_server(&app_uuid, &g_server_id);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] register server fail: 0x%x\r\n", ret);
        return ret;
    }

    if (sle_laser_add_service() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] add service fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (sle_laser_add_cmd_property() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] add cmd property fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (sle_laser_add_status_property() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] add status property fail\r\n");
        return ERRCODE_SLE_FAIL;
    }

    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] start service fail: 0x%x\r\n", ret);
        return ret;
    }

    osal_printk("[laser rx] SLE service registered OK\r\n");
    return ERRCODE_SLE_SUCCESS;
}

/* ================= 连接管理 ================= */
static void sle_connect_state_changed_cbk(uint16_t conn_id,
                                          const sle_addr_t *addr,
                                          sle_acb_state_t conn_state,
                                          sle_pair_state_t pair_state,
                                          sle_disc_reason_t disc_reason)
{
    unused(pair_state);
    unused(disc_reason);
    unused(addr);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_hdl = conn_id;
        g_last_accepted_seq = 0;
        g_heartbeat_rx_count = 0;
        g_last_heartbeat_log_ms = 0;
        g_last_status_report_ms = 0;
        osal_printk("[laser rx] SLE connected, conn_id=%u\r\n", conn_id);
        safety_set_sle_connected(true);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_conn_hdl = 0;
        g_last_accepted_seq = 0;
        g_heartbeat_rx_count = 0;
        g_last_heartbeat_log_ms = 0;
        g_last_status_report_ms = 0;
        osal_printk("[laser rx] SLE disconnected\r\n");
        safety_set_sle_connected(false);
        laser_enable(false);
        cmd_queue_flush();
        /* 重启广播 */
        sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    osal_printk("[laser rx] pair complete: 0x%x\r\n", status);
}

static void sle_auth_complete_cbk(uint16_t conn_id,
                                  const sle_addr_t *addr,
                                  errcode_t status,
                                  const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    osal_printk("[laser rx] auth complete: 0x%x\r\n", status);
}

static void sle_conn_register_cbks(void)
{
    sle_connection_callbacks_t cbk = {0};
    cbk.connect_state_changed_cb = sle_connect_state_changed_cbk;
    cbk.pair_complete_cb = sle_pair_complete_cbk;
    cbk.auth_complete_cb = sle_auth_complete_cbk;
    sle_connection_register_callbacks(&cbk);
}

/* ================= 广播 ================= */

#define SLE_ADV_LOCAL_NAME_LEN 7
static uint8_t g_announce_data[] = {
    SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
    1,
    SLE_ANNOUNCE_LEVEL_NORMAL,
    SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS,
    2,
    0x0B,
    0x1A,
};

static uint8_t g_scan_rsp_data[] = {SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
                                    1,
                                    10,
                                    SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME,
                                    SLE_ADV_LOCAL_NAME_LEN,
                                    'L',
                                    'a',
                                    's',
                                    'e',
                                    'r',
                                    'R',
                                    'X'};

static errcode_t sle_laser_adv_init(void)
{
    sle_announce_param_t param = {0};
    uint8_t mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
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
    memcpy_s(param.own_addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);
    errcode_t ret = sle_set_announce_param(param.announce_handle, &param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] set adv param fail: 0x%x\r\n", ret);
        return ret;
    }

    sle_announce_data_t data = {0};
    data.announce_data = g_announce_data;
    data.announce_data_len = sizeof(g_announce_data);
    data.seek_rsp_data = g_scan_rsp_data;
    data.seek_rsp_data_len = sizeof(g_scan_rsp_data);
    ret = sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] set adv data fail: 0x%x\r\n", ret);
        return ret;
    }

    return sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
}

/* ================= 广播回调 ================= */
static void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("[laser rx] adv enable id:%u, status:0x%x\r\n", announce_id, status);
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

static void sle_enable_cbk(errcode_t status)
{
    osal_printk("[laser rx] sle enable: 0x%x\r\n", status);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] sle enable failed\r\n");
        return;
    }
    sle_server_enable_cbk();
}

void sle_server_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t cbk = {0};
    cbk.announce_enable_cb = sle_announce_enable_cbk;
    cbk.announce_disable_cb = sle_announce_disable_cbk;
    cbk.announce_terminal_cb = sle_announce_terminal_cbk;
    cbk.sle_enable_cb = sle_enable_cbk;
    sle_announce_seek_register_callbacks(&cbk);
}

/* ================= 公共接口 ================= */
void sle_server_enable_cbk(void)
{
    errcode_t ret = sle_laser_server_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] server add failed: 0x%x\r\n", ret);
        return;
    }

    ssap_exchange_info_t info = {0};
    info.mtu_size = 512;
    info.version = 1;
    ret = ssaps_set_info(g_server_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] set ssap info fail: 0x%x\r\n", ret);
        return;
    }

    sle_addr_t addr = {0};
    uint8_t mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    addr.type = 0;
    memcpy_s(addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);
    ret = sle_set_local_addr(&addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] set local addr fail: 0x%x\r\n", ret);
        return;
    }

    ret = sle_laser_adv_init();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] adv init fail: 0x%x\r\n", ret);
        return;
    }
    osal_printk("[laser rx] SLE server init OK\r\n");
}

errcode_t sle_laser_server_init(void)
{
    sle_server_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();
    errcode_t ret = enable_sle();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser rx] enable_sle fail: 0x%x\r\n", ret);
        return ret;
    }
    osal_printk("[laser rx] SLE enable called\r\n");
    return ERRCODE_SLE_SUCCESS;
}

errcode_t sle_laser_server_send_status(const uint8_t *data, uint16_t len)
{
    ssaps_ntf_ind_t param = {0};
    param.handle = g_status_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value = (uint8_t *)data;
    param.value_len = len;
    return ssaps_notify_indicate(g_server_id, g_conn_hdl, &param);
}

uint16_t sle_laser_server_get_conn_id(void)
{
    return g_conn_hdl;
}
