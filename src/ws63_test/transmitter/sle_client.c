/**
 * @file sle_client.c
 * @brief 发射板 SLE Client 实现
 *        基于 sle_speed_client 示例改造
 *        扫描连接接收板 "LaserRX"，发送 motion_cmd_t，接收 status_pkt_t
 */
#include "sle_client.h"
#include "config.h"
#include "crc16.h"
#include "gcode_processor.h"
#include "systick.h"

#include "securec.h"
#include "soc_osal.h"
#include "common_def.h"
#include "sle_common.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include <string.h>

/* SLE 广播数据类型: 设备完整本地名称 (部分 SDK 版本未在公共头文件中导出此定义) */
#ifndef SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME
#define SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME 0x0B
#endif

#ifndef SLE_SEEK_ACTIVE
#define SLE_SEEK_ACTIVE 0x01
#endif

/* 当前链路默认不强制配对；若后续启用安全配对可改为 1 */
#ifndef LASER_TX_ENABLE_PAIRING
#define LASER_TX_ENABLE_PAIRING 0
#endif

/* 对齐接收板默认地址，名字匹配失败时使用 MAC 兜底命中 */
static const uint8_t g_target_addr[SLE_ADDR_LEN] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

/* ================= 状态变量 ================= */
static uint16_t g_conn_id = 0;
static bool g_connected = false;
static bool g_handles_ready = false;
static bool g_status_rx_seen = false;
/* 扫描命中目标后置位，等待 stop seek 完成回调里再 connect */
static bool g_connect_pending = false;
/* 由接收板状态包回传，用于发射板侧流控 */
static uint8_t g_remote_status = STATUS_IDLE;
static double g_remote_x = 0.0;
static double g_remote_y = 0.0;
static volatile uint32_t g_feedback_snapshot_seq = 0;
static uint8_t g_queue_free = 0;
static uint16_t g_last_ack_seq = 0;
static sle_addr_t g_remote_addr = {0};
static bool g_exchange_started = false;

static ssapc_find_service_result_t g_find_service_result = {0};
static uint16_t g_cmd_handle = 0;
static uint16_t g_status_handle = 0;
static volatile uint16_t g_pending_writes = 0;
static volatile uint32_t g_last_business_write_ms = 0;
static uint32_t g_write_req_count = 0;
static uint32_t g_write_cfm_ok_count = 0;
static uint32_t g_write_cfm_fail_count = 0;
static uint32_t g_write_submit_fail_count = 0;

static void sle_reset_runtime_state(void)
{
    g_handles_ready = false;
    g_status_rx_seen = false;
    g_remote_status = STATUS_IDLE;
    g_remote_x = 0.0;
    g_remote_y = 0.0;
    g_feedback_snapshot_seq = 0;
    g_queue_free = 0;
    g_last_ack_seq = 0;
    g_exchange_started = false;
    g_cmd_handle = 0;
    g_status_handle = 0;
    g_pending_writes = 0;
    g_last_business_write_ms = 0;
    g_write_req_count = 0;
    g_write_cfm_ok_count = 0;
    g_write_cfm_fail_count = 0;
    g_write_submit_fail_count = 0;
    memset(&g_find_service_result, 0, sizeof(g_find_service_result));
}

static bool sle_addr_match_target(const sle_addr_t *addr)
{
    return (addr != NULL) && (memcmp(addr->addr, g_target_addr, SLE_ADDR_LEN) == 0);
}

static void sle_start_service_discovery(uint16_t conn_id)
{
    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    ssapc_find_structure(0, conn_id, &find_param);
}

static void sle_request_exchange_info(uint16_t conn_id)
{
    if (g_exchange_started) {
        return;
    }
    g_exchange_started = true;

    ssap_exchange_info_t info = {0};
    info.mtu_size = 512;
    info.version = 1;
    errcode_t ret = ssapc_exchange_info_req(0, conn_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[laser tx] exchange info req fail: 0x%x, fallback discovery\r\n", ret);
        /* 兼容部分协议栈行为：交换信息失败时仍尝试继续服务发现 */
        sle_start_service_discovery(conn_id);
    }
}

static bool uuid16_equals(const sle_uuid_t *uuid, uint16_t expect)
{
    if (uuid == NULL || uuid->len != 2) {
        return false;
    }
    /* SDK 使用 128-bit uuid 缓冲，16-bit uuid 放在末尾两个字节 */
    return (uuid->uuid[14] == (uint8_t)(expect & 0xFF)) && (uuid->uuid[15] == (uint8_t)((expect >> 8) & 0xFF));
}

/* ================= 扫描回调 ================= */
static void sle_sample_sle_enable_cbk(errcode_t status)
{
    osal_printk("[laser tx] sle enable: 0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        /* 开始扫描 */
        sle_seek_param_t param = {0};
        param.own_addr_type = 0;
        param.filter_duplicates = 0;
        param.seek_filter_policy = 0;
        param.seek_phys = 1;
        /* 主动扫描可拿到 scan response，设备名 LaserRX 在接收板 scan response 里 */
        param.seek_type[0] = SLE_SEEK_ACTIVE;
        param.seek_interval[0] = 0x64;
        param.seek_window[0] = 0x64;
        sle_set_seek_param(&param);
        sle_start_seek();
        osal_printk("[laser tx] scanning...\r\n");
    }
}

static void sle_sample_seek_result_cbk(sle_seek_result_info_t *seek_result)
{
    if (seek_result == NULL)
        return;
    if (g_connect_pending || g_connected) {
        return;
    }

    if (sle_addr_match_target(&seek_result->addr)) {
        osal_printk("[laser tx] found target by MAC\r\n");
        memcpy_s(&g_remote_addr, sizeof(sle_addr_t), &seek_result->addr, sizeof(sle_addr_t));
        g_connect_pending = true;
        sle_stop_seek();
        return;
    }

    if (seek_result->data == NULL)
        return;

    /* 广播 TLV 解析: [type][len][payload...]，按条目线性扫描目标设备名 */
    uint8_t *data = seek_result->data;
    uint16_t len = seek_result->data_length;

    for (uint16_t i = 0; i < len;) {
        if (i + 1 >= len)
            break;
        uint8_t ad_type = data[i];
        uint8_t ad_len = data[i + 1];
        if ((uint16_t)(i + 2 + ad_len) > len) {
            break;
        }

        if (ad_type == SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME && ad_len >= 7) {
            if (memcmp(&data[i + 2], "LaserRX", 7) == 0) {
                osal_printk("[laser tx] found LaserRX!\r\n");
                memcpy_s(&g_remote_addr, sizeof(sle_addr_t), &seek_result->addr, sizeof(sle_addr_t));
                g_connect_pending = true;
                /* 先停扫，等 seek_disable 回调再发起连接，避免状态竞争 */
                sle_stop_seek();
                return;
            }
        }
        i += ad_len + 2;
    }
}

static void sle_sample_seek_enable_cbk(errcode_t status)
{
    osal_printk("[laser tx] seek enable: 0x%x\r\n", status);
}

static void sle_sample_seek_disable_cbk(errcode_t status)
{
    osal_printk("[laser tx] seek disable: 0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS && g_connect_pending) {
        g_connect_pending = false;
        /* 扫描关闭后再连接目标设备 */
        sle_connect_remote_device(&g_remote_addr);
    }
}

/* ================= 连接回调 ================= */
static void sle_connect_state_changed_cbk(uint16_t conn_id,
                                          const sle_addr_t *addr,
                                          sle_acb_state_t conn_state,
                                          sle_pair_state_t pair_state,
                                          sle_disc_reason_t disc_reason)
{
    unused(disc_reason);
    const uint8_t addr0 = (addr != NULL) ? addr->addr[0] : 0;
    const uint8_t addr4 = (addr != NULL) ? addr->addr[4] : 0;
    const uint8_t addr5 = (addr != NULL) ? addr->addr[5] : 0;

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_id = conn_id;
        g_connected = true;
        sle_reset_runtime_state();
        osal_printk("[laser tx] connected! conn_id=%u pair_state=0x%x addr:%02x:**:**:**:%02x:%02x\r\n", conn_id,
                    pair_state, addr0, addr4, addr5);

        /* 当前板级链路会返回 ERRCODE_SLE_PAIRING_REJECT(0x8000600f)，默认不主动配对 */
#if LASER_TX_ENABLE_PAIRING
        if (pair_state == SLE_PAIR_NONE) {
            errcode_t ret = sle_pair_remote_device(&g_remote_addr);
            if (ret != ERRCODE_SLE_SUCCESS) {
                osal_printk("[laser tx] pair request fail: 0x%x\r\n", ret);
            }
        }
#else
        if (pair_state == SLE_PAIR_NONE) {
            osal_printk("[laser tx] pairing disabled, continue without pair\r\n");
        }
#endif

        /* 无论是否配对成功，都先推进交换信息/服务发现，避免链路卡死 */
        sle_request_exchange_info(conn_id);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_connected = false;
        g_connect_pending = false;
        sle_reset_runtime_state();
        osal_printk("[laser tx] disconnected, restarting scan\r\n");
        /* 回调运行在 SLE service 线程，避免阻塞等待 */
        sle_start_seek();
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    const uint8_t addr0 = (addr != NULL) ? addr->addr[0] : 0;
    const uint8_t addr4 = (addr != NULL) ? addr->addr[4] : 0;
    const uint8_t addr5 = (addr != NULL) ? addr->addr[5] : 0;
    osal_printk("[laser tx] pair complete conn_id=%u addr:%02x:**:**:**:%02x:%02x status:0x%x\r\n", conn_id,
                addr0, addr4, addr5, status);
    if (status == ERRCODE_SLE_SUCCESS) {
        sle_request_exchange_info(conn_id);
    } else {
        osal_printk("[laser tx] pair failed/rejected, continue with exchange/discovery path\r\n");
        sle_request_exchange_info(conn_id);
    }
}

static void sle_auth_complete_cbk(uint16_t conn_id,
                                  const sle_addr_t *addr,
                                  errcode_t status,
                                  const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    osal_printk("[laser tx] auth complete: 0x%x\r\n", status);
}

/* ================= SSAP Client 回调 ================= */
static void sle_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || param == NULL) {
        osal_printk("[laser tx] exchange info failed: 0x%x\r\n", status);
        /* 回退到直接发现，避免链路停在交换阶段 */
        sle_start_service_discovery(conn_id);
        return;
    }
    osal_printk("[laser tx] MTU: %d\r\n", param->mtu_size);
    sle_start_service_discovery(conn_id);
}

static void sle_find_structure_cbk(uint8_t client_id,
                                   uint16_t conn_id,
                                   ssapc_find_service_result_t *service,
                                   errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(status);

    if (status != ERRCODE_SLE_SUCCESS || service == NULL) {
        return;
    }

    if (uuid16_equals(&service->uuid, SLE_LASER_SERVICE_UUID)) {
        memcpy_s(&g_find_service_result, sizeof(ssapc_find_service_result_t), service,
                 sizeof(ssapc_find_service_result_t));
        osal_printk("[laser tx] laser service found: start=0x%x end=0x%x\r\n", service->start_hdl, service->end_hdl);

        /* 仅在激光服务区间内继续发现属性，减少无关遍历 */
        ssapc_find_structure_param_t find_param = {0};
        find_param.type = SSAP_FIND_TYPE_PROPERTY;
        find_param.start_hdl = service->start_hdl;
        find_param.end_hdl = service->end_hdl;
        ssapc_find_structure(0, conn_id, &find_param);
    }
}

static void sle_find_property_cbk(uint8_t client_id,
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
        /* 发命令用写句柄 */
        g_cmd_handle = property->handle;
        osal_printk("[laser tx] cmd handle: 0x%x\r\n", g_cmd_handle);
    } else if (uuid16_equals(&property->uuid, SLE_LASER_STATUS_CHAR_UUID)) {
        /* 收状态用通知句柄 */
        g_status_handle = property->handle;
        osal_printk("[laser tx] status handle: 0x%x\r\n", g_status_handle);
    }

    g_handles_ready = (g_cmd_handle != 0) && (g_status_handle != 0);
}

static void sle_find_structure_cmp_cbk(uint8_t client_id,
                                       uint16_t conn_id,
                                       ssapc_find_structure_result_t *result,
                                       errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    if (status != ERRCODE_SLE_SUCCESS || result == NULL) {
        return;
    }

    if (result->type == SSAP_FIND_TYPE_PROPERTY) {
        osal_printk("[laser tx] discovery done, handles_ready=%d status_rx=%d\r\n", g_handles_ready ? 1 : 0,
                    g_status_rx_seen ? 1 : 0);
    }
}

static void sle_write_cfm_cbk(uint8_t client_id,
                              uint16_t conn_id,
                              ssapc_write_result_t *write_result,
                              errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(write_result);
    if (status == ERRCODE_SLE_SUCCESS) {
        g_write_cfm_ok_count++;
        if (g_pending_writes > 0) {
            g_pending_writes--;
        }
        return;
    }
    g_write_cfm_fail_count++;
    if (g_pending_writes > 0) {
        g_pending_writes--;
    }
    if ((g_write_cfm_fail_count % 20U) == 1U) {
        osal_printk("[laser tx] write cfm fail: 0x%x (ok=%u fail=%u)\r\n", status, g_write_cfm_ok_count,
                    g_write_cfm_fail_count);
    }
}

/**
 * @brief Notification 回调 — 接收接收板发来的 status_pkt_t
 */
static void sle_notification_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);

    if (status != ERRCODE_SLE_SUCCESS || data == NULL || data->data == NULL || data->data_len < sizeof(status_pkt_t)) {
        return;
    }

    if (g_status_handle == 0 || data->handle != g_status_handle) {
        return;
    }

    /* 新协议优先: status_full_pkt_t = 基础状态 + 反馈坐标 */
    if (data->data_len >= sizeof(status_full_pkt_t)) {
        status_full_pkt_t full = {0};
        memcpy(&full, data->data, sizeof(status_full_pkt_t));
        if (status_pkt_check_crc(&full.base)) {
            bool first_status = !g_status_rx_seen;
            g_feedback_snapshot_seq++;
            g_status_rx_seen = true;
            g_remote_status = full.base.status;
            g_last_ack_seq = full.base.ack_seq;
            g_queue_free = full.base.queue_free;
            g_remote_x = (double)full.cur_x;
            g_remote_y = (double)full.cur_y;
            /* 把接收板真实坐标回灌给 GCode 层，用于 '?' 状态上报 */
            gcode_processor_update_feedback_pos(full.cur_x, full.cur_y);
            g_feedback_snapshot_seq++;
            if (first_status) {
                osal_printk("[laser tx] status link ready\r\n");
            }
        }
        return;
    }

    /* 兼容旧协议: 仅基础状态 */
    status_pkt_t base = {0};
    memcpy(&base, data->data, sizeof(status_pkt_t));
    if (status_pkt_check_crc(&base)) {
        bool first_status = !g_status_rx_seen;
        g_feedback_snapshot_seq++;
        g_status_rx_seen = true;
        g_remote_status = base.status;
        g_last_ack_seq = base.ack_seq;
        g_queue_free = base.queue_free;
        g_feedback_snapshot_seq++;
        if (first_status) {
            osal_printk("[laser tx] status link ready\r\n");
        }
    }
}

static void sle_indication_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

/* ================= 初始化 ================= */
errcode_t sle_laser_client_init(void)
{
    /* 回调注册顺序不敏感，但应在 enable_sle 之前完成 */
    /* 注册扫描/广播回调 */
    sle_announce_seek_callbacks_t seek_cbk = {0};
    seek_cbk.sle_enable_cb = sle_sample_sle_enable_cbk;
    seek_cbk.seek_enable_cb = sle_sample_seek_enable_cbk;
    seek_cbk.seek_disable_cb = sle_sample_seek_disable_cbk;
    seek_cbk.seek_result_cb = sle_sample_seek_result_cbk;
    sle_announce_seek_register_callbacks(&seek_cbk);

    /* 注册连接回调 */
    sle_connection_callbacks_t conn_cbk = {0};
    conn_cbk.connect_state_changed_cb = sle_connect_state_changed_cbk;
    conn_cbk.pair_complete_cb = sle_pair_complete_cbk;
    conn_cbk.auth_complete_cb = sle_auth_complete_cbk;
    sle_connection_register_callbacks(&conn_cbk);

    /* 注册 SSAP Client 回调 */
    ssapc_callbacks_t ssapc_cbk = {0};
    ssapc_cbk.exchange_info_cb = sle_exchange_info_cbk;
    ssapc_cbk.find_structure_cb = sle_find_structure_cbk;
    ssapc_cbk.ssapc_find_property_cbk = sle_find_property_cbk;
    ssapc_cbk.find_structure_cmp_cb = sle_find_structure_cmp_cbk;
    ssapc_cbk.write_cfm_cb = sle_write_cfm_cbk;
    ssapc_cbk.notification_cb = sle_notification_cbk;
    ssapc_cbk.indication_cb = sle_indication_cbk;
    ssapc_register_callbacks(&ssapc_cbk);

    sle_reset_runtime_state();

    /* 使能 SLE */
    enable_sle();
    osal_printk("[laser tx] SLE enable called\r\n");

    return ERRCODE_SLE_SUCCESS;
}

errcode_t sle_laser_client_send_cmd(const motion_cmd_t *cmd)
{
    /* 统一做前置条件检查，避免上层重复判断遗漏 */
    if (cmd == NULL || g_cmd_handle == 0) {
        return ERRCODE_SLE_FAIL;
    }

    if (cmd->cmd == CMD_HEARTBEAT) {
        if (!sle_laser_client_can_send_heartbeat()) {
            return ERRCODE_SLE_FAIL;
        }
    } else if (!sle_laser_client_is_ready()) {
        return ERRCODE_SLE_FAIL;
    }

    uint16_t pending_limit = SLE_TX_BUSINESS_MAX_PENDING;
    if (cmd->cmd == CMD_HEARTBEAT) {
        pending_limit = SLE_TX_HEARTBEAT_MAX_PENDING;
    }
    if (g_pending_writes >= pending_limit) {
        return ERRCODE_SLE_BUSY;
    }

    ssapc_write_param_t param = {0};
    param.handle = g_cmd_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = sizeof(motion_cmd_t);
    param.data = (uint8_t *)cmd;

    errcode_t ret = ssapc_write_req(0, g_conn_id, &param);
    if (ret == ERRCODE_SLE_SUCCESS) {
        g_pending_writes++;
        if (cmd->cmd != CMD_HEARTBEAT) {
            g_last_business_write_ms = (uint32_t)uapi_systick_get_ms();
        }
        g_write_req_count++;
        return ret;
    }

    g_write_submit_fail_count++;
    if ((g_write_submit_fail_count % 20U) == 1U) {
        osal_printk("[laser tx] write submit fail: 0x%x (pending=%u submit_fail=%u)\r\n", ret, g_pending_writes,
                    g_write_submit_fail_count);
    }
    return ret;
}

bool sle_laser_client_is_connected(void)
{
    return g_connected;
}

bool sle_laser_client_can_send_heartbeat(void)
{
    return g_connected && g_handles_ready;
}

bool sle_laser_client_is_ready(void)
{
    return sle_laser_client_can_send_heartbeat() && g_status_rx_seen;
}

bool sle_laser_client_has_handles_ready(void)
{
    return g_handles_ready;
}

bool sle_laser_client_has_status_rx(void)
{
    return g_status_rx_seen;
}

uint8_t sle_laser_client_get_remote_status(void)
{
    return g_remote_status;
}

void sle_laser_client_get_feedback_snapshot(uint8_t *status, double *x, double *y)
{
    uint32_t seq_begin = 0;
    uint32_t seq_end = 0;
    uint8_t local_status = STATUS_IDLE;
    double local_x = 0.0;
    double local_y = 0.0;

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
