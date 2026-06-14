/**
 * @file sle_server.c
 * @brief Minimal SLE receiver input for the wireless laser marker tree.
 */
#include "sle_server.h"
#include "config.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "protocol.h"
#include "wireless_crc16.h"

#include "common_def.h"
#include "securec.h"
#include "sle_common.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_ssap_server.h"
#include "soc_osal.h"
#include "systick.h"
#include <math.h>
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

#define UUID_LEN_2 2
#define MIN_FEED_RATE_MM_MIN 0.1f
#define MAX_FEED_RATE_MM_MIN ((float)G0_FEED_RATE)
#define SLE_CONN_INVALID 0xFFFF

static uint16_t g_conn_hdl = SLE_CONN_INVALID;
static uint8_t g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_cmd_property_handle = 0;
static uint16_t g_status_property_handle = 0;
static uint16_t g_last_accepted_seq = 0;
static uint32_t g_heartbeat_rx_count = 0;
static uint32_t g_business_rx_count = 0;
static uint64_t g_last_heartbeat_log_ms = 0;
static uint64_t g_last_status_report_ms = 0;
static osal_semaphore g_safety_sem;
static bool g_safety_sem_ready = false;
static volatile bool g_safety_off_pending = false;
static volatile uint16_t g_safety_off_seq = 0;
static volatile bool g_pending_ack = false;
static volatile status_pkt_t g_pending_ack_pkt;

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
    out->uuid[14] = (uint8_t)(u2 & 0xFF);
    out->uuid[15] = (uint8_t)((u2 >> 8) & 0xFF);
}

static uint8_t wireless_queue_free_count(void)
{
    uint16_t depth = motion_executor_queue_depth();
    if (depth >= MOTION_QUEUE_SIZE) {
        return 0;
    }
    uint16_t free_count = (uint16_t)(MOTION_QUEUE_SIZE - depth);
    return (free_count > 255U) ? 255U : (uint8_t)free_count;
}

static uint8_t sle_runtime_status(void)
{
    if (motion_executor_is_busy() || motion_executor_queue_depth() > 0) {
        return STATUS_RUNNING;
    }
    return STATUS_IDLE;
}

static void mark_safety_off_pending(uint16_t seq)
{
    unsigned int irq_state = osal_irq_lock();
    g_safety_off_pending = true;
    g_safety_off_seq = seq;
    osal_irq_restore(irq_state);
    motion_executor_request_abort();
}

static void signal_safety_off(void)
{
    if (g_safety_sem_ready) {
        osal_sem_up(&g_safety_sem);
    }
}

static int safety_off_task(void *arg)
{
    unused(arg);
    osal_printk("[wireless rx] safety task started\r\n");

    while (1) {
        sle_laser_server_flush_pending_ack();
        if (osal_sem_down_timeout(&g_safety_sem, 500U) != OSAL_SUCCESS) {
        }

        uint16_t seq = 0;
        unsigned int irq_state = osal_irq_lock();
        if (g_safety_off_pending) {
            g_safety_off_pending = false;
            seq = g_safety_off_seq;
        }
        osal_irq_restore(irq_state);

        if (seq == 0U) {
            continue;
        }

        osal_printk("[wireless rx] safety off execute seq=%u\r\n", seq);
        motion_executor_request_abort();
        motion_executor_flush();
        laser_force_off();
    }

    return 0;
}

static errcode_t safety_off_task_start(void)
{
    if (!g_safety_sem_ready && osal_sem_init(&g_safety_sem, 0) != OSAL_SUCCESS) {
        osal_printk("[wireless rx] safety sem init failed\r\n");
        return ERRCODE_FAIL;
    }
    g_safety_sem_ready = true;

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(safety_off_task, NULL, "rx_safety", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[wireless rx] create safety task failed\r\n");
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_SLE) != OSAL_SUCCESS) {
        osal_printk("[wireless rx] set safety priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();
    return ERRCODE_SUCC;
}

static bool wireless_seq_is_new(uint16_t seq)
{
    if (g_last_accepted_seq == 0U) {
        return seq != 0U;
    }
    return (seq != g_last_accepted_seq) && ((uint16_t)(seq - g_last_accepted_seq) < 0x8000U);
}

static bool wireless_seq_is_duplicate(uint16_t seq)
{
    return (g_last_accepted_seq != 0U) && (seq == g_last_accepted_seq);
}

static int32_t milli_from_float(float value)
{
    float scaled = value * 1000.0f;
    return (int32_t)((scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f));
}

static void sle_send_ack_pkt(uint8_t status, uint8_t error_code, uint16_t ack_seq)
{
    status_pkt_t pkt = {0};
    pkt.status = status;
    pkt.error_code = error_code;
    pkt.ack_seq = ack_seq;
    pkt.queue_free = wireless_queue_free_count();
    status_pkt_set_crc(&pkt);

    unsigned int irq_state = osal_irq_lock();
    g_pending_ack_pkt = pkt;
    g_pending_ack = true;
    osal_irq_restore(irq_state);
}

void sle_laser_server_flush_pending_ack(void)
{
    if (!g_pending_ack) {
        return;
    }

    status_pkt_t pkt;
    unsigned int irq_state = osal_irq_lock();
    g_pending_ack = false;
    pkt = g_pending_ack_pkt;
    osal_irq_restore(irq_state);

    errcode_t ret = sle_laser_server_send_status((const uint8_t *)&pkt, sizeof(pkt));
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] flush ack fail: 0x%x\r\n", ret);
    }
}

static void sle_send_full_status_pkt(uint8_t status, uint8_t error_code, uint16_t ack_seq)
{
    status_full_pkt_t full = {0};
    full.base.status = status;
    full.base.error_code = error_code;
    full.base.ack_seq = ack_seq;
    full.base.queue_free = wireless_queue_free_count();
    full.cur_x = (float)motion_executor_get_x();
    full.cur_y = (float)motion_executor_get_y();
    status_pkt_set_crc(&full.base);

    errcode_t ret = sle_laser_server_send_status((const uint8_t *)&full, sizeof(full));
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] send full status fail: 0x%x\r\n", ret);
        return;
    }
    g_last_status_report_ms = uapi_systick_get_ms();
}

static bool is_finite_in_range(float val, float min_val, float max_val)
{
    return isfinite(val) && (val >= min_val) && (val <= max_val);
}

static bool validate_motion_cmd(const motion_cmd_t *cmd, uint8_t *error_code)
{
    if (cmd == NULL || error_code == NULL) {
        return false;
    }

    *error_code = STATUS_ERR_NONE;
    switch (cmd->cmd) {
        case CMD_G0_MOVE:
        case CMD_G1_MOVE:
            if (cmd->seq == 0U) {
                *error_code = STATUS_ERR_INVALID_PARAM;
                return false;
            }
            if (!is_finite_in_range(cmd->target_x, (float)GALVO_X_MIN_MM, (float)GALVO_X_MAX_MM) ||
                !is_finite_in_range(cmd->target_y, (float)GALVO_Y_MIN_MM, (float)GALVO_Y_MAX_MM) ||
                !is_finite_in_range(cmd->feed_rate, MIN_FEED_RATE_MM_MIN, MAX_FEED_RATE_MM_MIN)) {
                *error_code = STATUS_ERR_INVALID_PARAM;
                return false;
            }
            return true;

        case CMD_LASER_ON:
            if (cmd->seq == 0U) {
                *error_code = STATUS_ERR_INVALID_PARAM;
                return false;
            }
            if (cmd->laser_pwr > (uint16_t)LASER_S_MAX) {
                *error_code = STATUS_ERR_INVALID_PARAM;
                return false;
            }
            return true;

        case CMD_LASER_OFF:
        case CMD_SET_ORIGIN:
        case CMD_SET_MODE:
        case CMD_EMERGENCY_STOP:
            if (cmd->seq == 0U) {
                *error_code = STATUS_ERR_INVALID_PARAM;
                return false;
            }
            return true;
        case CMD_HEARTBEAT:
            return true;

        default:
            *error_code = STATUS_ERR_INVALID_CMD;
            return false;
    }
}

static void ssaps_read_request_cbk(uint8_t server_id,
                                   uint16_t conn_id,
                                   ssaps_req_read_cb_t *read_cb_para,
                                   errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(read_cb_para);
    unused(status);
    osal_printk("[wireless rx] read request\r\n");
}

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

    if (write_cb_para->length != sizeof(motion_cmd_t)) {
        osal_printk("[wireless rx] invalid cmd size: %u\r\n", write_cb_para->length);
        return;
    }

    motion_cmd_t cmd = {0};
    (void)memcpy_s(&cmd, sizeof(cmd), write_cb_para->value, sizeof(cmd));
    if (!motion_cmd_check_crc(&cmd)) {
        osal_printk("[wireless rx] CRC error seq=%u\r\n", cmd.seq);
        return;
    }

    uint8_t validate_err = STATUS_ERR_NONE;
    if (!validate_motion_cmd(&cmd, &validate_err)) {
        osal_printk("[wireless rx] invalid cmd=0x%x seq=%u err=%u\r\n", cmd.cmd, cmd.seq, validate_err);
        sle_send_ack_pkt(STATUS_ERROR, validate_err, g_last_accepted_seq);
        return;
    }

    if (cmd.cmd == CMD_HEARTBEAT) {
        g_heartbeat_rx_count++;
        uint64_t now = uapi_systick_get_ms();
        if ((g_last_heartbeat_log_ms == 0) || ((now - g_last_heartbeat_log_ms) >= 1000U)) {
            g_last_heartbeat_log_ms = now;
            osal_printk("[wireless rx] heartbeat rx=%u queue_free=%u\r\n", g_heartbeat_rx_count,
                        wireless_queue_free_count());
        }
        if ((g_last_status_report_ms == 0) ||
            ((now - g_last_status_report_ms) >= SLE_LASER_STATUS_REPORT_INTERVAL_MS)) {
            sle_send_full_status_pkt(sle_runtime_status(), STATUS_ERR_NONE, g_last_accepted_seq);
        }
        if (g_safety_off_pending) {
            signal_safety_off();
        }
        sle_laser_server_flush_pending_ack();
        return;
    }

    if (cmd.cmd == CMD_EMERGENCY_STOP) {
        osal_printk("[wireless rx] emergency stop seq=%u\r\n", cmd.seq);
        motion_executor_request_abort();
        motion_executor_flush();
        laser_force_off();
        g_last_accepted_seq = cmd.seq;
        sle_send_ack_pkt(STATUS_ERROR, STATUS_ERR_ESTOP, g_last_accepted_seq);
        return;
    }

    if (wireless_seq_is_duplicate(cmd.seq)) {
        osal_printk("[wireless rx] duplicate seq=%u cmd=0x%x; resend ack\r\n", cmd.seq, cmd.cmd);
        sle_send_ack_pkt(sle_runtime_status(), STATUS_ERR_NONE, g_last_accepted_seq);
        return;
    }

    if (!wireless_seq_is_new(cmd.seq)) {
        osal_printk("[wireless rx] stale seq=%u last=%u cmd=0x%x; resend ack\r\n", cmd.seq, g_last_accepted_seq,
                    cmd.cmd);
        sle_send_ack_pkt(sle_runtime_status(), STATUS_ERR_NONE, g_last_accepted_seq);
        return;
    }

    if (cmd.cmd == CMD_LASER_OFF) {
        osal_printk("[wireless rx] laser off seq=%u; ack first, defer force off\r\n", cmd.seq);
        mark_safety_off_pending(cmd.seq);
        g_last_accepted_seq = cmd.seq;
        g_business_rx_count++;
        sle_send_ack_pkt(STATUS_IDLE, STATUS_ERR_NONE, g_last_accepted_seq);
        return;
    }

    if (!motion_executor_enqueue_deferred(&cmd)) {
        osal_printk("[wireless rx] enqueue fail seq=%u depth=%u ready=%u worker=%u abort=%u busy=%u\r\n", cmd.seq,
                    motion_executor_queue_depth(), motion_executor_queue_ready() ? 1U : 0U,
                    motion_executor_worker_started() ? 1U : 0U,
                    motion_executor_abort_requested() ? 1U : 0U, motion_executor_is_busy() ? 1U : 0U);
        sle_send_ack_pkt(STATUS_ERROR, STATUS_ERR_QUEUE_FULL, g_last_accepted_seq);
        return;
    }

    g_last_accepted_seq = cmd.seq;
    g_business_rx_count++;
    if (g_business_rx_count <= 16U || ((g_business_rx_count % 64U) == 0U)) {
        osal_printk("[wireless rx] cmd rx=%u seq=%u cmd=0x%x x_milli=%d y_milli=%d f_milli=%d p=%u qfree=%u\r\n",
                    g_business_rx_count, cmd.seq, cmd.cmd, milli_from_float(cmd.target_x),
                    milli_from_float(cmd.target_y), milli_from_float(cmd.feed_rate), cmd.laser_pwr,
                    wireless_queue_free_count());
    }
    sle_send_ack_pkt(STATUS_RUNNING, STATUS_ERR_NONE, g_last_accepted_seq);
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id, ssap_exchange_info_t *mtu_size, errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(status);
    if (mtu_size != NULL) {
        osal_printk("[wireless rx] MTU changed: %u\r\n", mtu_size->mtu_size);
    }
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(handle);
    unused(status);
    osal_printk("[wireless rx] service started\r\n");
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
    (void)memcpy_s(app_uuid.uuid, app_uuid.len, app_uuid_data, sizeof(app_uuid_data));

    errcode_t ret = ssaps_register_server(&app_uuid, &g_server_id);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] register server fail: 0x%x\r\n", ret);
        return ret;
    }
    if (sle_laser_add_service() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] add service fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (sle_laser_add_cmd_property() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] add cmd property fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (sle_laser_add_status_property() != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] add status property fail\r\n");
        return ERRCODE_SLE_FAIL;
    }

    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] start service fail: 0x%x\r\n", ret);
        return ret;
    }
    osal_printk("[wireless rx] SLE service registered OK\r\n");
    return ERRCODE_SLE_SUCCESS;
}

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
        g_business_rx_count = 0;
        g_last_heartbeat_log_ms = 0;
        g_last_status_report_ms = 0;
        osal_printk("[wireless rx] SLE connected conn_id=%u\r\n", conn_id);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_conn_hdl = SLE_CONN_INVALID;
        g_last_accepted_seq = 0;
        g_heartbeat_rx_count = 0;
        g_business_rx_count = 0;
        g_last_heartbeat_log_ms = 0;
        g_last_status_report_ms = 0;
        osal_printk("[wireless rx] SLE disconnected; force laser off\r\n");
        motion_executor_request_abort();
        motion_executor_flush();
        laser_force_off();
        (void)sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    osal_printk("[wireless rx] pair complete: 0x%x\r\n", status);
}

static void sle_auth_complete_cbk(uint16_t conn_id,
                                  const sle_addr_t *addr,
                                  errcode_t status,
                                  const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(addr);
    unused(evt);
    osal_printk("[wireless rx] auth complete: 0x%x\r\n", status);
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
    0x0B,
    0x1A,
};

static uint8_t g_scan_rsp_data[] = {SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
                                    1,
                                    10,
                                    SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME,
                                    7,
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
    param.conn_interval_min = SLE_CONN_INTERVAL_MIN;
    param.conn_interval_max = SLE_CONN_INTERVAL_MAX;
    param.conn_max_latency = 0;
    param.conn_supervision_timeout = 0x1F4;
    param.announce_tx_power = 10;
    param.own_addr.type = 0;
    (void)memcpy_s(param.own_addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);

    errcode_t ret = sle_set_announce_param(param.announce_handle, &param);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] set adv param fail: 0x%x\r\n", ret);
        return ret;
    }

    sle_announce_data_t data = {0};
    data.announce_data = g_announce_data;
    data.announce_data_len = sizeof(g_announce_data);
    data.seek_rsp_data = g_scan_rsp_data;
    data.seek_rsp_data_len = sizeof(g_scan_rsp_data);
    ret = sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] set adv data fail: 0x%x\r\n", ret);
        return ret;
    }
    return sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
}

static void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("[wireless rx] adv enable id=%u status=0x%x\r\n", announce_id, status);
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

static void sle_server_enable_cbk(void);

static void sle_enable_cbk(errcode_t status)
{
    osal_printk("[wireless rx] sle enable: 0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        sle_server_enable_cbk();
    }
}

static void sle_server_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t cbk = {0};
    cbk.announce_enable_cb = sle_announce_enable_cbk;
    cbk.announce_disable_cb = sle_announce_disable_cbk;
    cbk.announce_terminal_cb = sle_announce_terminal_cbk;
    cbk.sle_enable_cb = sle_enable_cbk;
    sle_announce_seek_register_callbacks(&cbk);
}

static void sle_server_enable_cbk(void)
{
    errcode_t ret = sle_laser_server_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] server add failed: 0x%x\r\n", ret);
        return;
    }

    ssap_exchange_info_t info = {0};
    info.mtu_size = 512;
    info.version = 1;
    ret = ssaps_set_info(g_server_id, &info);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] set ssap info fail: 0x%x\r\n", ret);
        return;
    }

    sle_addr_t addr = {0};
    uint8_t mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    addr.type = 0;
    (void)memcpy_s(addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);
    ret = sle_set_local_addr(&addr);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] set local addr fail: 0x%x\r\n", ret);
        return;
    }

    ret = sle_laser_adv_init();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] adv init fail: 0x%x\r\n", ret);
        return;
    }
    osal_printk("[wireless rx] SLE server init OK\r\n");
}

errcode_t sle_laser_server_init(void)
{
    errcode_t ret = safety_off_task_start();
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    sle_server_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();

    ret = enable_sle();
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[wireless rx] enable_sle fail: 0x%x\r\n", ret);
        return ret;
    }
    osal_printk("[wireless rx] enable_sle called\r\n");
    return ERRCODE_SUCC;
}

errcode_t sle_laser_server_send_status(const uint8_t *data, uint16_t len)
{
    if (g_conn_hdl == SLE_CONN_INVALID || data == NULL || len == 0) {
        return ERRCODE_SLE_FAIL;
    }

    ssaps_ntf_ind_t ntf = {0};
    ntf.handle = g_status_property_handle;
    ntf.type = SSAP_PROPERTY_TYPE_VALUE;
    ntf.value = (uint8_t *)data;
    ntf.value_len = len;
    return ssaps_notify_indicate(g_server_id, g_conn_hdl, &ntf);
}

uint16_t sle_laser_server_get_conn_id(void)
{
    return g_conn_hdl;
}

uint32_t sle_laser_server_get_heartbeat_rx_count(void)
{
    return g_heartbeat_rx_count;
}

uint32_t sle_laser_server_get_business_rx_count(void)
{
    return g_business_rx_count;
}

uint16_t sle_laser_server_get_last_ack_seq(void)
{
    return g_last_accepted_seq;
}
