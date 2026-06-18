/**
 * @file main.c
 * @brief 发射板主入口 — 创建 UART / WiFi / SLE 任务
 */
#include "app_init.h"
#include "soc_osal.h"
#include "common_def.h"
#include "config.h"
#include "sle_errcode.h"
#include <string.h>

#include "gcode_processor.h"
#include "uart_handler.h"
#include "wifi_gcode_server.h"
#include "sle_client.h"
#include "crc16.h"
#include "focus_protocol.h"
#include "safety_protocol.h"
#include "systick.h"

/* 独立心跳任务：避免依赖 UART 空闲轮询导致保活间隔抖动 */
static int heartbeat_task(void *arg)
{
    (void)arg;
    motion_cmd_t hb = {0};
    uint32_t hb_ok_count = 0;
    uint32_t hb_busy_count = 0;
    uint32_t hb_fail_count = 0;
    uint64_t last_stat_log_ms = 0;

    while (1) {
        uint32_t sleep_ms = HEARTBEAT_INTERVAL_MS;
        uint32_t now = (uint32_t)uapi_systick_get_ms();
        uint32_t last_business_ms = sle_laser_client_get_last_business_write_ms();
        bool suppress_heartbeat = (last_business_ms != 0U) &&
                                  ((uint32_t)(now - last_business_ms) < SLE_TX_HEARTBEAT_SUPPRESS_AFTER_BUSINESS_MS);

        /* 业务在飞时仍允许发心跳；业务侧会主动给心跳预留窗口，避免接收板 safety timeout */
        if (sle_laser_client_can_send_heartbeat() && !suppress_heartbeat) {
            memset(&hb, 0, sizeof(hb));
            hb.cmd = CMD_HEARTBEAT;
            motion_cmd_set_crc(&hb);
            errcode_t ret = sle_laser_client_send_cmd(&hb);
            if (ret == ERRCODE_SUCC) {
                hb_ok_count++;
            } else if (ret == ERRCODE_SLE_BUSY) {
                hb_busy_count++;
                /* 心跳只做温和重试，避免高频争抢在途窗口把业务命令饿死。 */
                sleep_ms = SLE_TX_HEARTBEAT_BUSY_RETRY_INTERVAL_MS;
            } else {
                hb_fail_count++;
                if ((hb_fail_count % 20U) == 1U) {
                    osal_printk("[laser tx] heartbeat send fail: 0x%x (count=%u)\r\n", ret, hb_fail_count);
                }
            }
            if ((ret == ERRCODE_SUCC) && (hb_fail_count != 0U)) {
                osal_printk("[laser tx] heartbeat send recovered after %u fails\r\n", hb_fail_count);
                hb_fail_count = 0;
            }
        }

        if ((last_stat_log_ms == 0) || (((uint64_t)now - last_stat_log_ms) >= 5000)) {
            last_stat_log_ms = now;
            osal_printk(
                "[laser tx] hb stat peers=%u/%u conn=%u handles=%u status_rx=%u cmd_hdl=0x%x status_hdl=0x%x qfree=%u ack=%u "
                "ok=%u busy=%u fail=%u pending=%u wr_req=%u cfm_ok=%u cfm_fail=%u submit_fail=%u\r\n",
                sle_client_get_connected_peer_count(), sle_client_get_configured_peer_count(),
                sle_laser_client_is_connected() ? 1U : 0U, sle_laser_client_has_handles_ready() ? 1U : 0U,
                sle_laser_client_has_status_rx() ? 1U : 0U, sle_laser_client_get_cmd_handle(),
                sle_laser_client_get_status_handle(), sle_laser_client_get_queue_free(),
                sle_laser_client_get_last_ack_seq(), hb_ok_count, hb_busy_count, hb_fail_count,
                sle_laser_client_get_pending_writes(),
                sle_laser_client_get_write_req_count(), sle_laser_client_get_write_cfm_ok_count(),
                sle_laser_client_get_write_cfm_fail_count(), sle_laser_client_get_write_submit_fail_count());
        }
        osal_msleep(sleep_ms);
    }

    return 0;
}

static int focus_link_task(void *arg)
{
    uint16_t seq = 0;
    uint64_t last_query_ms = 0;
    uint64_t last_log_ms = 0;
    focus_node_cmd_t query = {0};
    focus_node_status_t status = {0};

    unused(arg);

    while (1) {
        uint64_t now = uapi_systick_get_ms();

        if (sle_focus_client_has_handles_ready() &&
            (!sle_focus_client_has_status_rx() || ((now - last_query_ms) >= ZDT_POLL_INTERVAL_MS))) {
            memset(&query, 0, sizeof(query));
            query.version = FOCUS_PROTOCOL_VERSION;
            query.cmd = FOCUS_CMD_QUERY_STATUS;
            query.seq = ++seq;
            focus_cmd_set_crc(&query);
            if (sle_focus_client_send_cmd(&query) == ERRCODE_SUCC) {
                last_query_ms = now;
            }
        }

        if ((last_log_ms == 0U) || ((now - last_log_ms) >= 5000U)) {
            last_log_ms = now;
            sle_focus_client_get_status_snapshot(&status);
            osal_printk(
                "[laser tx] focus stat peers=%u/%u conn=%u handles=%u status_rx=%u cmd_hdl=0x%x status_hdl=0x%x pending=%u "
                "state=%u err=%u flags=0x%02x pos=%ld speed=%d ack=%u\r\n",
                sle_client_get_connected_peer_count(), sle_client_get_configured_peer_count(),
                sle_focus_client_is_connected() ? 1U : 0U, sle_focus_client_has_handles_ready() ? 1U : 0U,
                sle_focus_client_has_status_rx() ? 1U : 0U, sle_focus_client_get_cmd_handle(),
                sle_focus_client_get_status_handle(), sle_focus_client_get_pending_writes(), status.status,
                status.error_code, status.flags, (long)status.z_position_pulses, (int)status.z_speed_rpm,
                status.ack_seq);
        }

        osal_msleep(200);
    }

    return 0;
}

static int safety_link_task(void *arg)
{
    uint64_t last_log_ms = 0;
    safety_node_status_t status = {0};

    unused(arg);

    while (1) {
        uint64_t now = uapi_systick_get_ms();

        if ((last_log_ms == 0U) || ((now - last_log_ms) >= 5000U)) {
            last_log_ms = now;
            sle_safety_client_get_status_snapshot(&status);
            osal_printk(
                "[laser tx] safety stat peers=%u/%u conn=%u handles=%u status_rx=%u cmd_hdl=0x%x status_hdl=0x%x pending=%u "
                "state=%u err=%u flags=0x%02x ack=%u\r\n",
                sle_client_get_connected_peer_count(), sle_client_get_configured_peer_count(),
                sle_safety_client_is_connected() ? 1U : 0U, sle_safety_client_has_handles_ready() ? 1U : 0U,
                sle_safety_client_has_status_rx() ? 1U : 0U, sle_safety_client_get_cmd_handle(),
                sle_safety_client_get_status_handle(), sle_safety_client_get_pending_writes(), status.status,
                status.error_code, status.flags, status.ack_seq);
        }

        osal_msleep(200);
    }

    return 0;
}

/* SLE 初始化任务 */
static int sle_init_task(void *arg)
{
    (void)arg;
    /* 延迟启动，避免系统刚起机时蓝牙协议栈资源尚未就绪 */
    osal_msleep(500);
    sle_laser_client_init();
    return 0;
}

static void transmitter_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser Marker - Transmitter Board\r\n");
    osal_printk("========================================\r\n");

    /* 1) 先初始化纯软件模块，再启动任务 */
    gcode_processor_init();
    uart_handler_init();
#if LASER_WIFI_SOFTAP_ENABLE
    if (wifi_gcode_server_init() != ERRCODE_SUCC) {
        osal_printk("[transmitter] wifi gcode init failed\r\n");
    }
#endif

    osal_printk("[transmitter] init OK\r\n");

    osal_task *task = NULL;

    osal_kthread_lock();

    /* 2) UART 任务:
     *    负责接收上位机文本指令、解析并经 SLE 下发 */
    task = osal_kthread_create(task_uart_rx_entry, NULL, "uart_rx", TASK_STACK_SIZE_DEFAULT);
    if (task != NULL) {
        /* UART 轮询优先级低于 SLE(3) 和心跳(3)，但仍需及时响应 */
        osal_kthread_set_priority(task, TASK_PRIO_UART);
        osal_kfree(task);
    }

#if LASER_WIFI_SOFTAP_ENABLE
    /* 3) WiFi 任务:
     *    参考官方 SoftAP / STA 例程新增 TCP 文本入口。
     *    它与 UART 并行存在，但不替代原来的串口联调入口。 */
    task = osal_kthread_create(task_wifi_gcode_entry, NULL, "wifi_gcode", TASK_STACK_SIZE_WIFI);
    if (task != NULL) {
        /* WiFi 侧只做上游接入，优先级保持低于 SLE/UART，避免抢占实时控制链路。 */
        osal_kthread_set_priority(task, TASK_PRIO_WIFI);
        osal_kfree(task);
    }
#endif

    /* 4) SLE 初始化任务:
     *    独立拉起扫描/连接流程，避免阻塞入口线程 */
    task = osal_kthread_create(sle_init_task, NULL, "sle_init", TASK_STACK_SIZE_SLE);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_SLE);
        osal_kfree(task);
    }

    /* 5) 心跳任务:
     *    周期发送 CMD_HEARTBEAT，维持接收板安全看门狗 */
    task = osal_kthread_create(heartbeat_task, NULL, "hb", TASK_STACK_SIZE_DEFAULT);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_SLE);
        osal_kfree(task);
    }

    /* 6) Focus 链路任务:
     *    周期查询感知节点状态，确保发射板 <-> focus_node 的命令/状态链路持续在线。 */
    task = osal_kthread_create(focus_link_task, NULL, "focus_link", TASK_STACK_SIZE_DEFAULT);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_SLE);
        osal_kfree(task);
    }

    /* 7) Safety 链路任务:
     *    低频打印安全终端节点状态，方便联调 LED_ON / LED_OFF 节点验证链路。 */
    task = osal_kthread_create(safety_link_task, NULL, "safety_link", TASK_STACK_SIZE_DEFAULT);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_SLE);
        osal_kfree(task);
    }

    osal_kthread_unlock();

    osal_printk("[transmitter] all tasks created\r\n");
}

/* 系统启动入口 */
app_run(transmitter_entry);
