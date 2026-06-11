/**
 * @file uart_handler.c
 * @brief UART input for the wireless transmitter.
 */
#include "uart_handler.h"
#include "config.h"
#include "gcode_processor.h"
#include "pinctrl.h"
#include "sle_client.h"
#include "sle_errcode.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define RX_LINE_MAX 128
#define UART_RX_BUF_SIZE 4096
#define UART_READ_TIMEOUT_MS 20
#define WIRELESS_READY_WAIT_MS 3000U
#define WIRELESS_READY_POLL_MS 20U
#define GRBL_RESET_CHAR 0x18

static char g_rx_line[RX_LINE_MAX];
static int g_rx_pos = 0;
static uint8_t g_uart_rx_buff[UART_RX_BUF_SIZE] = {0};
static uart_buffer_config_t g_uart_buffer_config = {.rx_buffer = g_uart_rx_buff, .rx_buffer_size = UART_RX_BUF_SIZE};
static motion_cmd_t g_tx_queue[TX_OUTBOUND_QUEUE_SIZE];
static uint16_t g_tx_queue_head = 0;
static uint16_t g_tx_queue_tail = 0;
static uint16_t g_tx_queue_depth = 0;
static osal_mutex g_tx_queue_mutex;
static osal_semaphore g_tx_queue_sem;
static bool g_tx_queue_ready = false;

static volatile uint32_t g_tx_queue_enq_count = 0;
static volatile uint32_t g_tx_queue_deq_count = 0;
static volatile uint32_t g_tx_queue_drop_count = 0;
static volatile uint32_t g_tx_sender_sent_count = 0;
static volatile uint32_t g_tx_sender_ack_count = 0;
static volatile uint32_t g_tx_sender_retry_count = 0;
static volatile uint32_t g_tx_sender_fail_count = 0;
static volatile uint32_t g_tx_sender_ack_timeout_count = 0;
static volatile uint32_t g_tx_ack_wait_last_ms = 0;
static volatile uint32_t g_tx_ack_wait_max_ms = 0;
static volatile uint8_t g_tx_sender_state = 0;
static volatile uint16_t g_tx_sender_inflight = 0;
static volatile uint16_t g_tx_sender_inflight_base_seq = 0;
static volatile uint32_t g_realtime_status_count = 0;
static volatile uint32_t g_startup_banner_count = 0;
static volatile uint32_t g_soft_reset_count = 0;
static volatile uint32_t g_ready_wait_timeout_count = 0;

enum {
    TX_SENDER_IDLE = 0,
    TX_SENDER_WAIT_READY = 1,
    TX_SENDER_WAIT_QFREE = 2,
    TX_SENDER_SEND = 3,
    TX_SENDER_WAIT_ACK = 4,
    TX_SENDER_RETRY = 5,
    TX_SENDER_FAIL = 6,
    TX_SENDER_DRAIN = 7,
};

static void uart_send_str(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    if (len > 0) {
        uapi_uart_write(LASER_UART_BUS, (const uint8_t *)str, len, 0);
    }
}

static void send_grbl_startup(const char *source)
{
    g_startup_banner_count++;
    osal_printk("[wireless tx uart] startup banner source=%s count=%u\r\n", source, g_startup_banner_count);
    uart_send_str("\r\nGrbl 1.1f ['$' for help]\r\n");
}

static void send_realtime_status(void)
{
    char buf[96];
    double x = 0.0;
    double y = 0.0;
    bool idle = true;

    if (sle_laser_client_has_status_rx()) {
        uint8_t status = STATUS_IDLE;
        sle_laser_client_get_feedback_snapshot(&status, &x, &y);
        idle = (status == STATUS_IDLE);
    } else {
        gcode_processor_get_feedback_pos(&x, &y);
        idle = gcode_processor_is_idle();
    }

    grbl_format_status(buf, sizeof(buf), x, y, idle ? 1 : 0);
    uart_send_str(buf);

    g_realtime_status_count++;
    if (g_realtime_status_count <= 8U || (g_realtime_status_count % 100U) == 0U) {
        osal_printk("[wireless tx uart] realtime ? count=%u\r\n", g_realtime_status_count);
    }
}

static void uart_log_link_state(const char *reason)
{
    osal_printk("[wireless tx uart] %s: conn=%u handles=%u srx=%u ready=%u qfree=%u ack=%u pending=%u age=%u\r\n",
                reason,
                sle_laser_client_is_connected() ? 1U : 0U, sle_laser_client_has_handles_ready() ? 1U : 0U,
                sle_laser_client_has_status_rx() ? 1U : 0U, sle_laser_client_is_ready() ? 1U : 0U,
                sle_laser_client_get_queue_free(), sle_laser_client_get_last_ack_seq(),
                sle_laser_client_get_pending_writes(), sle_laser_client_get_status_age_ms());
}

static bool wait_wireless_ready(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (!sle_laser_client_is_ready()) {
        if (waited_ms >= timeout_ms) {
            g_ready_wait_timeout_count++;
            uart_log_link_state("not_ready");
            return false;
        }
        osal_msleep(WIRELESS_READY_POLL_MS);
        waited_ms += WIRELESS_READY_POLL_MS;
    }
    return true;
}

static uint16_t tx_queue_depth_snapshot(void)
{
    uint16_t depth;
    if (!g_tx_queue_ready) {
        return 0;
    }
    osal_mutex_lock(&g_tx_queue_mutex);
    depth = g_tx_queue_depth;
    osal_mutex_unlock(&g_tx_queue_mutex);
    return depth;
}

static bool tx_queue_push(const motion_cmd_t *cmd)
{
    if (cmd == NULL || !g_tx_queue_ready) {
        return false;
    }

    osal_mutex_lock(&g_tx_queue_mutex);
    if (g_tx_queue_depth >= TX_OUTBOUND_QUEUE_SIZE) {
        g_tx_queue_drop_count++;
        osal_mutex_unlock(&g_tx_queue_mutex);
        return false;
    }

    g_tx_queue[g_tx_queue_tail] = *cmd;
    g_tx_queue_tail = (uint16_t)((g_tx_queue_tail + 1U) % TX_OUTBOUND_QUEUE_SIZE);
    g_tx_queue_depth++;
    g_tx_queue_enq_count++;
    osal_mutex_unlock(&g_tx_queue_mutex);
    osal_sem_up(&g_tx_queue_sem);
    return true;
}

static bool tx_queue_pop(motion_cmd_t *cmd)
{
    if (cmd == NULL || !g_tx_queue_ready) {
        return false;
    }
    if (osal_sem_down(&g_tx_queue_sem) != OSAL_SUCCESS) {
        return false;
    }

    osal_mutex_lock(&g_tx_queue_mutex);
    if (g_tx_queue_depth == 0U) {
        osal_mutex_unlock(&g_tx_queue_mutex);
        return false;
    }

    *cmd = g_tx_queue[g_tx_queue_head];
    g_tx_queue_head = (uint16_t)((g_tx_queue_head + 1U) % TX_OUTBOUND_QUEUE_SIZE);
    g_tx_queue_depth--;
    g_tx_queue_deq_count++;
    osal_mutex_unlock(&g_tx_queue_mutex);
    return true;
}

static bool tx_queue_try_pop(motion_cmd_t *cmd)
{
    if (cmd == NULL || !g_tx_queue_ready) {
        return false;
    }
    if (osal_sem_down_timeout(&g_tx_queue_sem, 0) != OSAL_SUCCESS) {
        return false;
    }

    osal_mutex_lock(&g_tx_queue_mutex);
    if (g_tx_queue_depth == 0U) {
        osal_mutex_unlock(&g_tx_queue_mutex);
        return false;
    }

    *cmd = g_tx_queue[g_tx_queue_head];
    g_tx_queue_head = (uint16_t)((g_tx_queue_head + 1U) % TX_OUTBOUND_QUEUE_SIZE);
    g_tx_queue_depth--;
    g_tx_queue_deq_count++;
    osal_mutex_unlock(&g_tx_queue_mutex);
    return true;
}

static void tx_queue_flush(void)
{
    if (!g_tx_queue_ready) {
        return;
    }
    osal_mutex_lock(&g_tx_queue_mutex);
    g_tx_queue_head = 0;
    g_tx_queue_tail = 0;
    g_tx_queue_depth = 0;
    while (osal_sem_down_timeout(&g_tx_queue_sem, 0) == OSAL_SUCCESS) {
    }
    osal_mutex_unlock(&g_tx_queue_mutex);
}

static bool send_cmd_with_retry(const motion_cmd_t *cmd)
{
    uint32_t waited_ms = 0;
    while (sle_laser_client_can_send_heartbeat() && sle_laser_client_get_pending_writes() > 0U) {
        if (waited_ms >= SLE_TX_BUSY_RETRY_TIMEOUT_MS) {
            osal_printk("[wireless tx uart] pending drain timeout cmd=0x%x pending=%u\r\n", cmd->cmd,
                        sle_laser_client_get_pending_writes());
            g_tx_sender_fail_count++;
            return false;
        }
        (void)sle_laser_client_wait_write_idle(SLE_TX_BUSY_RETRY_INTERVAL_MS);
        waited_ms += SLE_TX_BUSY_RETRY_INTERVAL_MS;
    }
    waited_ms = 0;
    while (sle_laser_client_can_send_heartbeat()) {
        errcode_t ret = sle_laser_client_send_cmd(cmd);
        if (ret == ERRCODE_SUCC) {
            return true;
        }
        if (ret != ERRCODE_SLE_BUSY) {
            osal_printk("[wireless tx uart] send fail cmd=0x%x ret=0x%x\r\n", cmd->cmd, ret);
            uart_log_link_state("send_fail");
            g_tx_sender_fail_count++;
            return false;
        }
        if (waited_ms >= SLE_TX_BUSY_RETRY_TIMEOUT_MS) {
            osal_printk("[wireless tx uart] busy timeout cmd=0x%x\r\n", cmd->cmd);
            g_tx_sender_fail_count++;
            return false;
        }
        osal_msleep(SLE_TX_BUSY_RETRY_INTERVAL_MS);
        waited_ms += SLE_TX_BUSY_RETRY_INTERVAL_MS;
    }
    return false;
}

static bool wait_cmd_remote_ack(uint16_t seq)
{
    uint32_t start_ms = (uint32_t)uapi_systick_get_ms();
    bool ok = sle_laser_client_wait_ack(seq, CMD_ACK_TIMEOUT_MS);
    uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - start_ms;
    g_tx_ack_wait_last_ms = elapsed;
    if (elapsed > g_tx_ack_wait_max_ms) {
        g_tx_ack_wait_max_ms = elapsed;
    }
    if (ok) {
        return true;
    }

    osal_printk("[wireless tx uart] remote ack timeout seq=%u app_ack=%u waited=%u\r\n", seq,
                sle_laser_client_get_last_ack_seq(), elapsed);
    uart_log_link_state("ack_timeout");
    g_tx_sender_ack_timeout_count++;
    return false;
}

static bool submit_business_cmd(const motion_cmd_t *cmd)
{
    if (!send_cmd_with_retry(cmd)) {
        return false;
    }
    g_tx_sender_sent_count++;
    return true;
}

static bool send_business_cmd_and_wait_ack(const motion_cmd_t *cmd)
{
    for (uint32_t attempt = 0; attempt <= CMD_RETRY_MAX; attempt++) {
        if (attempt > 0U) {
            g_tx_sender_retry_count++;
            g_tx_sender_state = TX_SENDER_RETRY;
        }
        if (!submit_business_cmd(cmd)) {
            return false;
        }
        g_tx_sender_state = TX_SENDER_WAIT_ACK;
        if (wait_cmd_remote_ack(cmd->seq)) {
            g_tx_sender_ack_count++;
            return true;
        }
    }
    return false;
}

static void handle_estop(void)
{
    motion_cmd_t cmd;
    tx_queue_flush();
    gcode_processor_build_emergency_stop(&cmd);
    if (!submit_business_cmd(&cmd)) {
        uart_send_str("error:estop_failed\r\n");
        return;
    }
    if (!wait_cmd_remote_ack(cmd.seq)) {
        uart_send_str("error:estop_ack_failed\r\n");
        return;
    }
    g_tx_sender_ack_count++;
    uart_send_str("ok\r\n");
}

static void process_line(const char *line, int len)
{
    if (len == 0) {
        return;
    }

    if (line[0] != '?') {
        osal_printk("[wireless tx uart] line rx len=%d text=%s\r\n", len, line);
    }

    if (strcmp(line, "!") == 0 || strcmp(line, "$STOP") == 0 || strcmp(line, "M112") == 0) {
        handle_estop();
        return;
    }

    if (line[0] == '?') {
        send_realtime_status();
        return;
    }

    char response[1024];
    if (strcmp(line, "$D") == 0) {
        snprintf(response, sizeof(response),
                 "[MSG:wireless tx conn=%u handles=%u ready=%u srx=%u sage=%u status=%u qfree=%u ack=%u pending=%u wr=%u ok=%u]\r\n"
                 "[MSG:fail=%u submit_fail=%u notify=%u txq=%u enq=%u deq=%u sent=%u acked=%u retry=%u drop=%u sender=%u ifl=%u ibase=%u ackms=%u ackmax=%u ackto=%u]\r\n"
                 "[MSG:rt=%u startup=%u reset=%u ready_to=%u]\r\nok\r\n",
                 sle_laser_client_is_connected() ? 1U : 0U, sle_laser_client_has_handles_ready() ? 1U : 0U,
                 sle_laser_client_is_ready() ? 1U : 0U, sle_laser_client_has_status_rx() ? 1U : 0U,
                 sle_laser_client_get_status_age_ms(), sle_laser_client_get_remote_status(),
                 sle_laser_client_get_queue_free(), sle_laser_client_get_last_ack_seq(),
                 sle_laser_client_get_pending_writes(), sle_laser_client_get_write_req_count(),
                 sle_laser_client_get_write_cfm_ok_count(), sle_laser_client_get_write_cfm_fail_count(),
                 sle_laser_client_get_write_submit_fail_count(), sle_laser_client_get_notify_rx_count(),
                 tx_queue_depth_snapshot(), g_tx_queue_enq_count, g_tx_queue_deq_count, g_tx_sender_sent_count,
                 g_tx_sender_ack_count, g_tx_sender_retry_count, g_tx_queue_drop_count, g_tx_sender_state,
                 g_tx_sender_inflight, g_tx_sender_inflight_base_seq, g_tx_ack_wait_last_ms,
                 g_tx_ack_wait_max_ms, g_tx_sender_ack_timeout_count, g_realtime_status_count,
                 g_startup_banner_count, g_soft_reset_count, g_ready_wait_timeout_count);
        uart_send_str(response);
        return;
    }
    if (grbl_process_dollar(line, response, sizeof(response))) {
        uart_send_str(response);
        return;
    }

    motion_cmd_t cmds[4];
    int cmd_count = 0;
    if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        bool has_critical = false;
        if (cmd_count > 0) {
            if (!wait_wireless_ready(WIRELESS_READY_WAIT_MS)) {
                uart_send_str("[MSG:wireless target not ready]\r\nerror:not_ready\r\n");
                return;
            }
            sle_laser_client_note_business_activity();
        }
        for (int i = 0; i < cmd_count; i++) {
            if (cmds[i].cmd == CMD_LASER_OFF) {
                has_critical = true;
                break;
            }
        }
        if (has_critical) {
            tx_queue_flush();
            for (int i = 0; i < cmd_count; i++) {
                if (!send_business_cmd_and_wait_ack(&cmds[i])) {
                    uart_send_str("error:send_failed\r\n");
                    return;
                }
            }
        } else {
            for (int i = 0; i < cmd_count; i++) {
                if (!tx_queue_push(&cmds[i])) {
                    uart_send_str("error:tx_queue_full\r\n");
                    return;
                }
            }
        }
    }

    uart_send_str("ok\r\n");
}

int task_tx_sender_entry(void *arg)
{
    unused(arg);
    osal_printk("[wireless tx sender] task started\r\n");

    motion_cmd_t cmd;
    motion_cmd_t batch_cmd;
    while (1) {
        g_tx_sender_state = TX_SENDER_IDLE;
        g_tx_sender_inflight = 0;
        g_tx_sender_inflight_base_seq = 0;

        g_tx_sender_state = TX_SENDER_WAIT_READY;
        while (!sle_laser_client_is_ready()) {
            osal_msleep(20);
        }

        g_tx_sender_state = TX_SENDER_WAIT_QFREE;
        while (sle_laser_client_has_status_rx() && sle_laser_client_get_queue_free() < FLOW_CTRL_PAUSE_THRESHOLD) {
            osal_msleep(5);
        }

        if (!tx_queue_pop(&cmd)) {
            osal_msleep(1);
            continue;
        }

        g_tx_sender_state = TX_SENDER_SEND;
        g_tx_sender_inflight = 0;
        g_tx_sender_inflight_base_seq = cmd.seq;
        uint16_t last_seq = cmd.seq;

        if (!submit_business_cmd(&cmd)) {
            g_tx_sender_state = TX_SENDER_FAIL;
            osal_printk("[wireless tx sender] send failed seq=%u cmd=0x%x\r\n", cmd.seq, cmd.cmd);
            osal_msleep(50);
            continue;
        }
        g_tx_sender_inflight++;

        while (g_tx_sender_inflight < SLE_TX_BUSINESS_MAX_PENDING) {
            if (sle_laser_client_has_status_rx() && sle_laser_client_get_queue_free() < FLOW_CTRL_PAUSE_THRESHOLD) {
                break;
            }
            if (!tx_queue_try_pop(&batch_cmd)) {
                break;
            }
            if (!submit_business_cmd(&batch_cmd)) {
                g_tx_sender_state = TX_SENDER_FAIL;
                osal_printk("[wireless tx sender] send failed seq=%u cmd=0x%x\r\n", batch_cmd.seq, batch_cmd.cmd);
                break;
            }
            last_seq = batch_cmd.seq;
            g_tx_sender_inflight++;
        }

        g_tx_sender_state = TX_SENDER_DRAIN;
        if (wait_cmd_remote_ack(last_seq)) {
            g_tx_sender_ack_count += g_tx_sender_inflight;
        } else {
            osal_printk("[wireless tx sender] batch ack timeout base=%u last=%u inflight=%u remote=%u\r\n",
                        g_tx_sender_inflight_base_seq, last_seq, g_tx_sender_inflight,
                        sle_laser_client_get_last_ack_seq());
        }
    }
    return 0;
}

int task_uart_rx_entry(void *arg)
{
    unused(arg);
    osal_printk("[wireless tx uart] task started\r\n");
    osal_msleep(100);
    send_grbl_startup("boot");

    uint8_t ch;
    while (1) {
        int32_t ret = uapi_uart_read(LASER_UART_BUS, &ch, 1, UART_READ_TIMEOUT_MS);
        if (ret <= 0) {
            osal_msleep(1);
            continue;
        }
        if (ch == GRBL_RESET_CHAR) {
            g_soft_reset_count++;
            osal_printk("[wireless tx uart] ctrl-x soft reset count=%u\r\n", g_soft_reset_count);
            g_rx_pos = 0;
            tx_queue_flush();
            send_grbl_startup("ctrl-x");
            continue;
        }
        if (ch == '?') {
            send_realtime_status();
            continue;
        }
        if (ch == '!' || ch == '~') {
            osal_printk("[wireless tx uart] realtime control 0x%x ignored\r\n", ch);
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (g_rx_pos > 0) {
                g_rx_line[g_rx_pos] = '\0';
                process_line(g_rx_line, g_rx_pos);
                g_rx_pos = 0;
            }
        } else if (g_rx_pos < RX_LINE_MAX - 1) {
            g_rx_line[g_rx_pos++] = (char)ch;
        }
    }
    return 0;
}

errcode_t uart_handler_init(void)
{
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(LASER_UART_RX_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(LASER_UART_TX_PIN, LASER_UART_PIN_MODE);
    uapi_pin_set_mode(LASER_UART_RX_PIN, LASER_UART_PIN_MODE);

    uart_attr_t attr = {0};
    attr.baud_rate = UART_BAUD_RATE;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    uart_pin_config_t pin_cfg = {0};
    pin_cfg.tx_pin = LASER_UART_TX_PIN;
    pin_cfg.rx_pin = LASER_UART_RX_PIN;
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(LASER_UART_BUS);
    if (!g_tx_queue_ready) {
        if (osal_mutex_init(&g_tx_queue_mutex) != OSAL_SUCCESS || osal_sem_init(&g_tx_queue_sem, 0) != OSAL_SUCCESS) {
            osal_printk("[wireless tx uart] queue init failed\r\n");
            return ERRCODE_FAIL;
        }
        g_tx_queue_ready = true;
    }
    errcode_t ret = uapi_uart_init(LASER_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buffer_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[wireless tx uart] init failed: 0x%x\r\n", ret);
        return ret;
    }
    osal_printk("[wireless tx uart] init OK bus=%d baud=%d\r\n", LASER_UART_BUS, UART_BAUD_RATE);
    return ERRCODE_SUCC;
}
