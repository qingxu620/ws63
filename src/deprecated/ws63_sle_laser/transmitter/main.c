/**
 * @file main.c
 * @brief Transmitter entry - local Grbl-compatible front-end over SLE motion link.
 */
#include "app_init.h"
#include "common_def.h"
#include "crc16.h"
#include "errcode.h"
#include "gcode_processor.h"
#include "pinctrl.h"
#include "sle_passthrough.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart.h"
#include <stdio.h>
#include <string.h>

#define TX_UART_BUS 1
#define TX_UART_TX_PIN 15
#define TX_UART_RX_PIN 16
#define TX_UART_PIN_MODE 1
#define TX_UART_BAUD 115200

#define RX_BUF_SIZE 1024
#define LINE_MAX 128
#define READ_TIMEOUT_MS 20
#define GRBL_RESET_CHAR 0x18

#define CMD_ACK_TIMEOUT_MS 1500U
#define CMD_RETRY_MAX 2U
#define LINK_READY_TIMEOUT_MS 5000U
#define HEARTBEAT_INTERVAL_MS 250U
#define HOST_SESSION_IDLE_RESET_MS 60000U
#define FLOW_CTRL_PAUSE_THRESHOLD 4U

static uint8_t g_uart_rx_buf[RX_BUF_SIZE];
static uart_buffer_config_t g_uart_buf_cfg = {
    .rx_buffer = g_uart_rx_buf,
    .rx_buffer_size = RX_BUF_SIZE
};

static char g_line[LINE_MAX];
static int g_line_pos = 0;
static bool g_line_overflow = false;
static bool g_session_startup_sent = false;
static uint32_t g_last_uart_rx_ms = 0;

static volatile bool g_status_rx_seen = false;
static volatile uint8_t g_remote_status = STATUS_IDLE;
static volatile uint8_t g_remote_error = STATUS_ERR_NONE;
static volatile uint8_t g_remote_queue_free = 255;
static volatile uint16_t g_last_ack_seq = 0;
static volatile uint32_t g_status_counter = 0;
static uint16_t g_tx_seq = 1;
static float g_remote_x = 0.0f;
static float g_remote_y = 0.0f;

static void uart_send_str(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    if (len > 0) {
        uapi_uart_write(TX_UART_BUS, (const uint8_t *)str, len, 0);
    }
}

static void send_grbl_startup(void)
{
    uart_send_str("\r\nGrbl 1.1f ['$' for help]\r\n");
    uart_send_str("[MSG:WS63 SLE Laser ready]\r\n");
    g_session_startup_sent = true;
}

static void reset_host_session_if_idle(void)
{
    uint32_t now = (uint32_t)uapi_systick_get_ms();
    if (g_last_uart_rx_ms != 0U &&
        (uint32_t)(now - g_last_uart_rx_ms) > HOST_SESSION_IDLE_RESET_MS) {
        g_session_startup_sent = false;
    }
    g_last_uart_rx_ms = now;
}

static bool seq_reached(uint16_t ack_seq, uint16_t target_seq)
{
    return (uint16_t)(ack_seq - target_seq) < 0x8000U;
}

static uint16_t next_tx_seq(void)
{
    if (g_tx_seq == 0U) {
        g_tx_seq = 1U;
    }
    return g_tx_seq++;
}

static void update_remote_status_from_base(const status_pkt_t *base)
{
    if (base == NULL || !status_pkt_check_crc(base)) {
        return;
    }

    g_remote_status = base->status;
    g_remote_error = base->error_code;
    g_remote_queue_free = base->queue_free;
    g_last_ack_seq = base->ack_seq;
    g_status_rx_seen = true;
    g_status_counter++;
}

static void on_sle_response(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length == 0) {
        return;
    }

    if (length >= sizeof(status_full_pkt_t)) {
        status_full_pkt_t full;
        memcpy(&full, data, sizeof(full));
        if (!status_pkt_check_crc(&full.base)) {
            return;
        }
        g_remote_x = full.cur_x;
        g_remote_y = full.cur_y;
        update_remote_status_from_base(&full.base);
        return;
    }

    if (length >= sizeof(status_pkt_t)) {
        status_pkt_t base;
        memcpy(&base, data, sizeof(base));
        update_remote_status_from_base(&base);
    }
}

static bool wait_link_ready(uint32_t timeout_ms)
{
    uint32_t start = (uint32_t)uapi_systick_get_ms();

    while ((uint32_t)((uint32_t)uapi_systick_get_ms() - start) < timeout_ms) {
        if (sle_passthrough_is_connected()) {
            return true;
        }
        osal_msleep(10);
    }

    return sle_passthrough_is_connected();
}

static bool wait_queue_window(void)
{
    uint32_t waited = 0;

    while (sle_passthrough_is_connected() && g_status_rx_seen &&
           g_remote_queue_free < FLOW_CTRL_PAUSE_THRESHOLD) {
        if (waited >= CMD_ACK_TIMEOUT_MS) {
            return false;
        }
        osal_msleep(5);
        waited += 5;
    }

    return true;
}

static bool wait_cmd_ack(uint16_t seq)
{
    uint32_t start = (uint32_t)uapi_systick_get_ms();

    while ((uint32_t)((uint32_t)uapi_systick_get_ms() - start) < CMD_ACK_TIMEOUT_MS) {
        if (seq_reached(g_last_ack_seq, seq)) {
            return true;
        }
        if (!sle_passthrough_is_connected()) {
            return false;
        }
        osal_msleep(2);
    }

    return false;
}

static bool send_motion_cmd_reliably(motion_cmd_t *cmd)
{
    if (cmd == NULL) {
        return false;
    }

    if (!wait_link_ready(LINK_READY_TIMEOUT_MS)) {
        return false;
    }

    if (!wait_queue_window()) {
        return false;
    }

    cmd->seq = next_tx_seq();
    motion_cmd_set_crc(cmd);
    for (uint32_t attempt = 0; attempt <= CMD_RETRY_MAX; attempt++) {
        errcode_t ret = sle_passthrough_send_line((const char *)cmd, (uint16_t)sizeof(*cmd));
        if (ret == ERRCODE_SUCC && wait_cmd_ack(cmd->seq)) {
            return true;
        }
        osal_msleep(20);
    }

    return false;
}

static void send_status_report(void)
{
    char buf[128];
    uint8_t status = g_status_rx_seen ? g_remote_status : STATUS_IDLE;
    const char *state = (status == STATUS_RUNNING) ? "Run" : "Idle";

    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d|Ln:%lu>\r\n",
             state, (double)g_remote_x, (double)g_remote_y,
             (int)gcode_processor_get_feed_rate(),
             (int)gcode_processor_get_laser_power(),
             gcode_processor_get_line_count());
    uart_send_str(buf);
}

static void send_debug_report(void)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[MSG:tx link=%d status_rx=%d status=%u err=%u qfree=%u ack=%u scnt=%lu x=%.3f y=%.3f]\r\nok\r\n",
             sle_passthrough_is_connected() ? 1 : 0,
             g_status_rx_seen ? 1 : 0,
             (unsigned int)g_remote_status,
             (unsigned int)g_remote_error,
             (unsigned int)g_remote_queue_free,
             (unsigned int)g_last_ack_seq,
             (unsigned long)g_status_counter,
             (double)g_remote_x,
             (double)g_remote_y);
    uart_send_str(buf);
}

static bool handle_dollar_command(const char *line)
{
    if (line == NULL || line[0] != '$') {
        return false;
    }

    if (strcmp(line, "$I") == 0) {
        uart_send_str("[VER:1.1f.WS63_SLE_TX:]\r\n[OPT:V,15,128]\r\nok\r\n");
        return true;
    }

    if (strcmp(line, "$G") == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n",
                 gcode_processor_is_absolute_mode() ? 90 : 91,
                 gcode_processor_laser_is_enabled() ? 3 : 5,
                 (int)gcode_processor_get_feed_rate(),
                 (int)gcode_processor_get_laser_power());
        uart_send_str(buf);
        return true;
    }

    if (strcmp(line, "$D") == 0) {
        send_debug_report();
        return true;
    }

    if (strcmp(line, "$$") == 0) {
        uart_send_str("$0=10\r\n$1=25\r\n$2=0\r\n$3=0\r\n$4=0\r\n$5=0\r\n$6=0\r\n");
        uart_send_str("$10=1\r\n$11=0.010\r\n$12=0.002\r\n$13=0\r\n$20=0\r\n$21=0\r\n$22=0\r\n$23=0\r\n");
        uart_send_str("$24=25.000\r\n$25=500.000\r\n$26=250\r\n$27=1.000\r\n$30=1000\r\n$31=0\r\n$32=0\r\nok\r\n");
        return true;
    }

    if (strcmp(line, "$") == 0) {
        uart_send_str("$G - View gcode parser state\r\n");
        uart_send_str("$I - View build info\r\n");
        uart_send_str("$D - View SLE debug state\r\n");
        uart_send_str("$X - Kill alarm lock\r\n");
        uart_send_str("$H - Set origin\r\n");
        uart_send_str("ok\r\n");
        return true;
    }

    if (strcmp(line, "$X") == 0 || strcmp(line, "$C") == 0) {
        uart_send_str("ok\r\n");
        return true;
    }

    if (strcmp(line, "$H") == 0) {
        motion_cmd_t cmds[2];
        int cmd_count = 0;
        gcode_processor_set_origin();
        if (gcode_process_line("G92", 3, cmds, 2, &cmd_count) && cmd_count > 0 &&
            send_motion_cmd_reliably(&cmds[0])) {
            g_remote_x = 0.0f;
            g_remote_y = 0.0f;
            uart_send_str("ok\r\n");
        } else {
            uart_send_str("error:2\r\n");
        }
        return true;
    }

    if (strncmp(line, "$J=", 3) == 0) {
        motion_cmd_t cmds[4];
        int cmd_count = 0;
        const char *jog = &line[3];
        if (gcode_process_line(jog, (int)strlen(jog), cmds, 4, &cmd_count)) {
            for (int i = 0; i < cmd_count; i++) {
                if (!send_motion_cmd_reliably(&cmds[i])) {
                    uart_send_str("error:2\r\n");
                    return true;
                }
            }
        }
        uart_send_str("ok\r\n");
        return true;
    }

    uart_send_str("ok\r\n");
    return true;
}

static void handle_emergency_stop(void)
{
    motion_cmd_t cmd;
    g_line_pos = 0;
    g_line_overflow = false;
    gcode_processor_build_emergency_stop(&cmd);
    (void)send_motion_cmd_reliably(&cmd);
}

static void process_line(const char *line, int len)
{
    if (line == NULL || len == 0) {
        return;
    }

    if (handle_dollar_command(line)) {
        return;
    }

    if (strcmp(line, "?") == 0) {
        send_status_report();
        return;
    }

    if (strcmp(line, "!") == 0 || strcmp(line, "$STOP") == 0 || strcmp(line, "M112") == 0) {
        handle_emergency_stop();
        uart_send_str("ok\r\n");
        return;
    }

    motion_cmd_t cmds[4];
    int cmd_count = 0;
    if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        for (int i = 0; i < cmd_count; i++) {
            if (!send_motion_cmd_reliably(&cmds[i])) {
                uart_send_str("error:2\r\n");
                return;
            }
        }
    }

    uart_send_str("ok\r\n");
}

static void process_char(uint8_t ch)
{
    if (ch == '?') {
        send_status_report();
        return;
    }

    if (ch == GRBL_RESET_CHAR) {
        handle_emergency_stop();
        gcode_processor_init();
        g_status_rx_seen = false;
        g_last_ack_seq = 0;
        send_grbl_startup();
        return;
    }

    if (ch == '!' || ch == '~') {
        return;
    }

    if (ch == '\n' || ch == '\r') {
        if (g_line_overflow) {
            g_line_overflow = false;
            g_line_pos = 0;
            uart_send_str("error:1\r\n");
            return;
        }

        if (g_line_pos > 0) {
            g_line[g_line_pos] = '\0';
            process_line(g_line, g_line_pos);
            g_line_pos = 0;
        }
        return;
    }

    if (g_line_pos < LINE_MAX - 1) {
        g_line[g_line_pos++] = (char)ch;
    } else {
        g_line_overflow = true;
    }
}

static int uart_rx_task(void *arg)
{
    unused(arg);
    osal_printk("[tx] uart rx task started\r\n");
    osal_msleep(300);
    send_grbl_startup();

    uint8_t ch;
    while (1) {
        int32_t ret = uapi_uart_read(TX_UART_BUS, &ch, 1, READ_TIMEOUT_MS);
        if (ret <= 0) {
            osal_msleep(1);
            continue;
        }

        reset_host_session_if_idle();
        if (!g_session_startup_sent && ch != GRBL_RESET_CHAR) {
            send_grbl_startup();
        }
        process_char(ch);
    }

    return 0;
}

static int sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    sle_passthrough_set_response_cb(on_sle_response);
    errcode_t ret = sle_passthrough_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[tx] SLE init failed: 0x%x\r\n", ret);
    }
    return 0;
}

static int heartbeat_task(void *arg)
{
    unused(arg);
    while (1) {
        sle_passthrough_poll_connect();
        if (sle_passthrough_is_connected()) {
            motion_cmd_t hb;
            memset(&hb, 0, sizeof(hb));
            hb.cmd = CMD_HEARTBEAT;
            motion_cmd_set_crc(&hb);
            (void)sle_passthrough_send_line((const char *)&hb, (uint16_t)sizeof(hb));
        }
        osal_msleep(HEARTBEAT_INTERVAL_MS);
    }
    return 0;
}

static errcode_t uart_init(void)
{
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(TX_UART_RX_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(TX_UART_TX_PIN, TX_UART_PIN_MODE);
    uapi_pin_set_mode(TX_UART_RX_PIN, TX_UART_PIN_MODE);

    uart_attr_t attr = {0};
    attr.baud_rate = TX_UART_BAUD;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    uart_pin_config_t pin_cfg = {0};
    pin_cfg.tx_pin = TX_UART_TX_PIN;
    pin_cfg.rx_pin = TX_UART_RX_PIN;
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(TX_UART_BUS);
    errcode_t ret = uapi_uart_init(TX_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buf_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[tx] uart init failed: 0x%x\r\n", ret);
        return ret;
    }
    osal_printk("[tx] uart init OK bus=%d baud=%d\r\n", TX_UART_BUS, TX_UART_BAUD);
    return ERRCODE_SUCC;
}

static void sle_transmitter_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 SLE Laser Transmitter (Grbl front-end)\r\n");
    osal_printk("========================================\r\n");

    gcode_processor_init();

    errcode_t ret = uart_init();
    if (ret != ERRCODE_SUCC) {
        return;
    }

    osal_kthread_lock();

    osal_task *task = osal_kthread_create(sle_init_task, NULL, "sle_init", 0x2000);
    if (task != NULL) {
        osal_kthread_set_priority(task, 26);
        osal_kfree(task);
    }

    task = osal_kthread_create(uart_rx_task, NULL, "uart_rx", 0x4000);
    if (task != NULL) {
        osal_kthread_set_priority(task, 3);
        osal_kfree(task);
    }

    task = osal_kthread_create(heartbeat_task, NULL, "hb", 0x2000);
    if (task != NULL) {
        osal_kthread_set_priority(task, 24);
        osal_kfree(task);
    }

    osal_kthread_unlock();
    osal_printk("[tx] ready\r\n");
}

app_run(sle_transmitter_entry);
