/**
 * @file uart_handler.c
 * @brief UART 处理器实现
 *        接收上位机 (LaserGRBL) 发送的 G-Code
 *        解析后通过 SLE 发送给接收板
 *        实现 Grbl 协议兼容 (ok 回复, ? 状态查询)
 */
#include "uart_handler.h"
#include "config.h"
#include "protocol.h"
#include "gcode_parser.h"
#include "gcode_processor.h"
#include "sle_client.h"
#include "sle_errcode.h"
#include "crc16.h"
#include "soc_osal.h"
#include "uart.h"
#include "pinctrl.h"
#include <string.h>
#include <stdio.h>

/* UART 接收缓冲 */
#define RX_LINE_MAX 128
static char g_rx_line[RX_LINE_MAX];
static int g_rx_pos = 0;

/* UART 驱动内部接收缓冲区 (必须提供给 uapi_uart_init) */
#define UART_RX_BUF_SIZE 256
static uint8_t g_uart_rx_buff[UART_RX_BUF_SIZE] = {0};
static uart_buffer_config_t g_uart_buffer_config = {.rx_buffer = g_uart_rx_buff, .rx_buffer_size = UART_RX_BUF_SIZE};
#define UART_READ_TIMEOUT_MS 20

/* UART 端口 — 由 config.h 定义 */

/* ================= UART 发送 ================= */
static void uart_send_str(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    if (len > 0) {
        uapi_uart_write(LASER_UART_BUS, (const uint8_t *)str, len, 0);
    }
}

static void uart_log_link_state(const char *reason, const motion_cmd_t *cmd)
{
    osal_printk(
        "[uart] %s: cmd=0x%x conn=%u handles=%u status_rx=%u cmd_hdl=0x%x status_hdl=0x%x pending=%u queue_free=%u "
        "ack=%u\r\n",
        reason, (cmd != NULL) ? cmd->cmd : 0, sle_laser_client_is_connected() ? 1U : 0U,
        sle_laser_client_has_handles_ready() ? 1U : 0U, sle_laser_client_has_status_rx() ? 1U : 0U,
        sle_laser_client_get_cmd_handle(), sle_laser_client_get_status_handle(),
        sle_laser_client_get_pending_writes(), sle_laser_client_get_queue_free(), sle_laser_client_get_last_ack_seq());
}

static bool uart_send_motion_cmd_with_retry(const motion_cmd_t *cmd)
{
    uint32_t waited_ms = 0;

    while (sle_laser_client_is_ready()) {
        errcode_t ret = sle_laser_client_send_cmd(cmd);
        if (ret == ERRCODE_SUCC) {
            return true;
        }
        if (ret != ERRCODE_SLE_BUSY) {
            osal_printk("[uart] sle send fail: cmd=0x%x ret=0x%x\r\n", cmd->cmd, ret);
            uart_log_link_state("sle send fail state", cmd);
            return false;
        }

        if (waited_ms >= SLE_TX_BUSY_RETRY_TIMEOUT_MS) {
            osal_printk("[uart] sle busy timeout: cmd=0x%x waited=%ums\r\n", cmd->cmd, waited_ms);
            uart_log_link_state("sle busy timeout state", cmd);
            return false;
        }

        osal_msleep(SLE_TX_BUSY_RETRY_INTERVAL_MS);
        waited_ms += SLE_TX_BUSY_RETRY_INTERVAL_MS;
    }

    return false;
}

static bool uart_seq_reached(uint16_t ack_seq, uint16_t target_seq)
{
    return (uint16_t)(ack_seq - target_seq) < 0x8000U;
}

static bool uart_wait_cmd_ack(uint16_t seq)
{
    uint32_t waited_ms = 0;

    while (sle_laser_client_is_ready()) {
        if (uart_seq_reached(sle_laser_client_get_last_ack_seq(), seq)) {
            return true;
        }
        if (waited_ms >= CMD_ACK_TIMEOUT_MS) {
            osal_printk("[uart] ack timeout: seq=%u last_ack=%u\r\n", seq, sle_laser_client_get_last_ack_seq());
            return false;
        }
        osal_msleep(SLE_TX_BUSY_RETRY_INTERVAL_MS);
        waited_ms += SLE_TX_BUSY_RETRY_INTERVAL_MS;
    }

    return false;
}

static bool uart_send_business_cmd_reliably(const motion_cmd_t *cmd)
{
    for (uint32_t attempt = 0; attempt <= CMD_RETRY_MAX; attempt++) {
        if (!uart_send_motion_cmd_with_retry(cmd)) {
            return false;
        }
        if (uart_wait_cmd_ack(cmd->seq)) {
            return true;
        }
        osal_printk("[uart] retry cmd seq=%u cmd=0x%x attempt=%u/%u\r\n", cmd->seq, cmd->cmd, attempt + 1,
                    CMD_RETRY_MAX + 1);
    }

    uart_log_link_state("ack wait timeout", cmd);
    return false;
}

/* ================= 处理一行 G-Code ================= */
static void process_line(const char *line, int len)
{
    if (len == 0)
        return;

    /* '?' 实时命令 — 状态查询 */
    if (line[0] == '?') {
        char buf[80];
        double cur_x = 0.0;
        double cur_y = 0.0;
        /* 优先使用接收板真实状态，只有在回程状态尚未建立时才退回本地活动超时逻辑 */
        bool is_idle;
        if (!sle_laser_client_is_connected()) {
            gcode_processor_get_feedback_pos(&cur_x, &cur_y);
            is_idle = true;
        } else if (sle_laser_client_has_status_rx()) {
            uint8_t remote_status = STATUS_IDLE;
            sle_laser_client_get_feedback_snapshot(&remote_status, &cur_x, &cur_y);
            is_idle = (remote_status == STATUS_IDLE);
        } else {
            gcode_processor_get_feedback_pos(&cur_x, &cur_y);
            is_idle = gcode_processor_is_idle();
        }
        grbl_format_status(buf, sizeof(buf), cur_x, cur_y, 0, 0, is_idle);
        uart_send_str(buf);
        return;
    }

    /* '$' Grbl 配置/查询命令，走本地应答路径，不下发接收板 */
    char response[128];
    if (grbl_process_dollar(line, response, sizeof(response))) {
        uart_send_str(response);
        return;
    }

    /* G-Code 处理 */
    motion_cmd_t cmds[4];
    int cmd_count = 0;

    if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        bool send_ok = true;
        for (int i = 0; i < cmd_count; i++) {
            if (!sle_laser_client_is_ready()) {
                uart_log_link_state("reject cmd not ready", &cmds[i]);
                send_ok = false;
                break;
            }

            /* 流控: 接收板队列水位低于阈值时暂停，避免无线链路拥塞 */
            int retry = 0;
            while (sle_laser_client_is_ready() && sle_laser_client_get_queue_free() < FLOW_CTRL_PAUSE_THRESHOLD) {
                osal_msleep(5);
                retry++;
                if (retry > 200) {
                    uart_log_link_state("queue_free wait timeout", &cmds[i]);
                    send_ok = false;
                    break; /* 1s 超时 */
                }
            }
            if (!send_ok) {
                break;
            }

            /* 发送命令:
             * SSAP 在途写请求窗口被打满时，会短暂返回 BUSY。
             * 这里改为限时重试，避免上位机批量发送时瞬时拥塞直接变成 error:2。 */
            if (!uart_send_business_cmd_reliably(&cmds[i])) {
                send_ok = false;
                break;
            }
        }

        if (!send_ok) {
            uart_send_str("error:2\r\n");
            return;
        }
    }

    /* 回复 "ok" — Grbl 协议要求每条命令都回复 */
    uart_send_str("ok\r\n");
}

/* ================= UART 接收任务 ================= */
int task_uart_rx_entry(void *arg)
{
    (void)arg;

    osal_printk("[uart] task started\r\n");

    /* 发送 Grbl 启动信息 */
    osal_msleep(500);
    uart_send_str("\r\nWS63 Laser Marker V1.0\r\n");
    uart_send_str("Grbl 1.1f ['$' for help]\r\n");

    uint8_t ch; /* 按字节读取，再拼成一行 */
    while (1) {
        /* 读取一个字节 */
        int32_t ret = uapi_uart_read(LASER_UART_BUS, &ch, 1, UART_READ_TIMEOUT_MS);
        if (ret <= 0) {
            /* 关键: uapi_uart_read 超时并不真正让出 CPU，必须手动 yield
             * 否则同优先级的 sle_init / hb 任务永远无法调度 → 100% CPU → NMI */
            osal_msleep(1);
            continue;
        }

        /* 行结束符 */
        if (ch == '\n' || ch == '\r') {
            if (g_rx_pos > 0) {
                g_rx_line[g_rx_pos] = '\0';
                process_line(g_rx_line, g_rx_pos);
                g_rx_pos = 0;
            }
        } else {
            if (g_rx_pos < RX_LINE_MAX - 1) {
                g_rx_line[g_rx_pos++] = (char)ch;
            } else {
                /* 超长行直接截断，等待换行后继续解析下一行 */
            }
        }
    }

    return 0;
}

errcode_t uart_handler_init(void)
{
    /* 1. 配置 UART 引脚复用与输入使能 */
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(LASER_UART_RX_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(LASER_UART_TX_PIN, LASER_UART_PIN_MODE); /* GPIO15 → UART1_TXD */
    uapi_pin_set_mode(LASER_UART_RX_PIN, LASER_UART_PIN_MODE); /* GPIO16 → UART1_RXD */

    /* 2. UART 初始化:
     *    g_uart_buffer_config 是驱动内部 DMA/缓存区，不等于业务行缓存 */
    uart_attr_t attr = {0};
    attr.baud_rate = UART_BAUD_RATE;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    uart_pin_config_t pin_cfg = {0};
    pin_cfg.tx_pin = LASER_UART_TX_PIN; /* GPIO15 — Pin 9  */
    pin_cfg.rx_pin = LASER_UART_RX_PIN; /* GPIO16 — Pin 10 */
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(LASER_UART_BUS);
    errcode_t ret = uapi_uart_init(LASER_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buffer_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[uart] init failed: 0x%x\r\n", ret);
        return ret;
    }

    osal_printk("[uart] init OK, bus=%d, TX=GPIO%d, RX=GPIO%d, baud=%d\r\n", LASER_UART_BUS, LASER_UART_TX_PIN,
                LASER_UART_RX_PIN, UART_BAUD_RATE);
    return ERRCODE_SUCC;
}
