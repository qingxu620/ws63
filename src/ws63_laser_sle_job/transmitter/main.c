/**
 * @file main.c
 * @brief TX board: UART job input to structured SLE packets.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "crc16.h"
#include "errcode.h"
#include "packet.h"
#include "pinctrl.h"
#include "protocol.h"
#include "sle_errcode.h"
#include "sle_job_client.h"
#include "soc_osal.h"
#include "uart.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TX_LINE_MAX 96
#define TX_PAYLOAD_BUF_SIZE SLE_JOB_PACKET_MAX_PAYLOAD
#define TX_DATA_RX_LOG_STEP 32U
#define TX_UART_IDLE_LOG_TICKS 50U
#define TX_UART_IDLE0_LOG_TICKS 1000U

static uint8_t g_uart_rx_buf[JOB_TX_UART_RX_BUF_SIZE];
static uart_buffer_config_t g_uart_cfg = {
    .rx_buffer = g_uart_rx_buf,
    .rx_buffer_size = JOB_TX_UART_RX_BUF_SIZE,
};

static osal_semaphore g_ack_sem;
static bool g_ack_sem_ready = false;
static volatile uint16_t g_wait_ack_seq = 0;
static volatile uint8_t g_wait_status = JOB_STATUS_INTERNAL_ERROR;
static volatile bool g_wait_got_ack = false;
static volatile bool g_wait_active = false;
static uint16_t g_tx_seq = 1;

static uint32_t g_job_id = 0;
static uint32_t g_job_total = 0;
static uint32_t g_job_offset = 0;
static uint16_t g_job_crc = 0;
static bool g_data_mode = false;
static uint8_t g_job_chunk[JOB_TX_DATA_CHUNK_MAX];
static uint16_t g_job_chunk_len = 0;
static uint32_t g_data_log_next = TX_DATA_RX_LOG_STEP;
static char g_line[TX_LINE_MAX];
static uint16_t g_line_len = 0;

static uint16_t next_seq(void)
{
    uint16_t seq = g_tx_seq++;
    if (g_tx_seq == 0) {
        g_tx_seq = 1;
    }
    return seq;
}

static void host_sendf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    (void)uapi_uart_write(LASER_UART_BUS, (const uint8_t *)buf, (uint32_t)n, 0);
}

static uint16_t payload_len_for_type(uint8_t type, const void *payload, uint16_t fallback)
{
    if (type == PKT_JOB_DATA && payload != NULL) {
        const job_data_payload_t *p = (const job_data_payload_t *)payload;
        return (uint16_t)(sizeof(job_data_payload_t) + p->data_len);
    }
    return fallback;
}

static errcode_t send_packet_wait_ack(uint8_t type, const void *payload, uint16_t payload_len)
{
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = next_seq();
    uint16_t actual_payload_len = payload_len_for_type(type, payload, payload_len);

    if (!sle_packet_encode(type, 0, seq, payload, actual_payload_len,
                           packet, sizeof(packet), &packet_len)) {
        osal_printk("[JOB_TX] encode fail type=0x%02x len=%u\r\n", type, actual_payload_len);
        return ERRCODE_FAIL;
    }

    while (g_ack_sem_ready && osal_sem_down_timeout(&g_ack_sem, 0) == OSAL_SUCCESS) {
    }
    g_wait_ack_seq = seq;
    g_wait_status = JOB_STATUS_INTERNAL_ERROR;
    g_wait_got_ack = false;
    g_wait_active = true;

    osal_printk("[JOB_TX_SEND_ENTER] type=0x%02x seq=%u payload=%u packet=%u conn=%u status=%s\r\n",
                type, seq, actual_payload_len, packet_len,
                (unsigned int)sle_job_client_is_connected(), sle_job_client_get_status());

    for (uint32_t retry = 0; retry <= JOB_TX_RETRY_MAX; retry++) {
        uint32_t reconnect_wait = 0;
        while (!sle_job_client_is_connected()) {
            if ((reconnect_wait % 50U) == 0U) {
                osal_printk("[JOB_TX_WAIT_CONN] type=0x%02x seq=%u retry=%u waited=%u status=%s\r\n",
                            type, seq, (unsigned int)retry, (unsigned int)reconnect_wait,
                            sle_job_client_get_status());
            }
            sle_job_client_poll_connect();
            osal_msleep(20);
            reconnect_wait++;
        }

        if (g_wait_got_ack && g_wait_status == JOB_STATUS_OK) {
            osal_printk("[JOB_TX_ACK_LATE] type=0x%02x seq=%u status=%u before_retry=%u\r\n",
                        type, seq, g_wait_status, (unsigned int)retry);
            g_wait_active = false;
            g_wait_ack_seq = 0;
            return ERRCODE_SUCC;
        }

        osal_printk("[JOB_TX_SEND_CALL] type=0x%02x seq=%u try=%u packet=%u status=%s\r\n",
                    type, seq, (unsigned int)retry, packet_len, sle_job_client_get_status());
        errcode_t ret = sle_job_client_send_packet(packet, packet_len);
        osal_printk("[JOB_TX_FRAME] type=0x%02x seq=%u len=%u try=%u ret=0x%x\r\n",
                    type, seq, packet_len, (unsigned int)retry, ret);
        if (ret == ERRCODE_SLE_SUCCESS) {
            if (osal_sem_down_timeout(&g_ack_sem, JOB_TX_ACK_TIMEOUT_MS) == OSAL_SUCCESS &&
                g_wait_got_ack) {
                if (g_wait_status == JOB_STATUS_OK) {
                    g_wait_active = false;
                    g_wait_ack_seq = 0;
                    return ERRCODE_SUCC;
                }
                break;
            }
            osal_printk("[JOB_TX_ACK_TIMEOUT] type=0x%02x seq=%u try=%u got=%u status=%u\r\n",
                        type, seq, (unsigned int)retry, (unsigned int)g_wait_got_ack,
                        (unsigned int)g_wait_status);
        }
    }

    g_wait_active = false;
    g_wait_ack_seq = 0;
    host_sendf("@NACK type=%u seq=%u status=%u\r\n", type, seq, g_wait_status);
    return ERRCODE_FAIL;
}

static void response_cb(const uint8_t *data, uint16_t length)
{
    sle_packet_view_t pkt;
    if (!sle_packet_decode(data, length, &pkt)) {
        osal_printk("[JOB_TX] bad response len=%u\r\n", length);
        return;
    }

    if ((pkt.type == PKT_ACK || pkt.type == PKT_NACK) && pkt.len == sizeof(ack_payload_t)) {
        ack_payload_t ack;
        memcpy(&ack, pkt.payload, sizeof(ack));
        osal_printk("[JOB_TX_ACK] ack_type=0x%02x ack_seq=%u status=%u job=%u off=%u credit=%u\r\n",
                    ack.ack_type, ack.ack_seq, ack.status, (unsigned int)ack.job_id,
                    (unsigned int)ack.offset, (unsigned int)ack.credit);
        if (!g_wait_active || ack.ack_seq != g_wait_ack_seq) {
            osal_printk("[JOB_TX_ACK_OLD] ack_type=0x%02x ack_seq=%u wait=%u status=%u ignored\r\n",
                        ack.ack_type, ack.ack_seq, g_wait_ack_seq, ack.status);
            return;
        }
        g_wait_status = ack.status;
        g_wait_got_ack = true;
        if (g_ack_sem_ready) {
            osal_sem_up(&g_ack_sem);
        }
        host_sendf("@ACK type=%u seq=%u status=%u offset=%u\r\n",
                   ack.ack_type, ack.ack_seq, ack.status, (unsigned int)ack.offset);
        return;
    }

    if (pkt.type == PKT_STATUS_RESP && pkt.len == sizeof(status_resp_payload_t)) {
        status_resp_payload_t st;
        memcpy(&st, pkt.payload, sizeof(st));
        osal_printk("[JOB_TX_STATUS] state=%u status=%u job=%u rx=%u/%u free=%u lines=%u\r\n",
                    st.state, st.status, (unsigned int)st.job_id,
                    (unsigned int)st.received_size, (unsigned int)st.total_size,
                    (unsigned int)st.cache_free, (unsigned int)st.executed_lines);
        host_sendf("@STATUS state=%u status=%u job=%u rx=%u total=%u free=%u lines=%u\r\n",
                   st.state, st.status, (unsigned int)st.job_id,
                   (unsigned int)st.received_size, (unsigned int)st.total_size,
                   (unsigned int)st.cache_free, (unsigned int)st.executed_lines);
    }
}

static errcode_t send_job_begin(uint32_t job_id, uint32_t total_size, uint16_t crc)
{
    job_begin_payload_t begin = {0};
    begin.job_id = job_id;
    begin.total_size = total_size;
    begin.job_crc16 = crc;
    return send_packet_wait_ack(PKT_JOB_BEGIN, &begin, sizeof(begin));
}

static errcode_t send_job_end(void)
{
    job_end_payload_t end = {0};
    end.job_id = g_job_id;
    end.total_size = g_job_total;
    end.job_crc16 = g_job_crc;
    return send_packet_wait_ack(PKT_JOB_END, &end, sizeof(end));
}

static errcode_t send_job_data_chunk(uint32_t offset, const uint8_t *data, uint16_t chunk_len)
{
    if (chunk_len == 0 || data == NULL) {
        return ERRCODE_SUCC;
    }

    uint8_t payload[TX_PAYLOAD_BUF_SIZE];
    job_data_payload_t *p = (job_data_payload_t *)payload;
    p->job_id = g_job_id;
    p->offset = offset;
    p->data_len = chunk_len;
    memcpy(p->data, data, chunk_len);

    errcode_t ret = send_packet_wait_ack(PKT_JOB_DATA, payload,
                                         (uint16_t)(sizeof(job_data_payload_t) + chunk_len));
    if (ret == ERRCODE_SUCC) {
        osal_printk("[JOB_TX_DATA_SENT] job=%u off=%u len=%u next=%u/%u\r\n",
                    (unsigned int)g_job_id, (unsigned int)offset,
                    (unsigned int)chunk_len, (unsigned int)(offset + chunk_len),
                    (unsigned int)g_job_total);
    }
    return ret;
}

static errcode_t flush_job_chunk(void)
{
    if (g_job_chunk_len == 0) {
        return ERRCODE_SUCC;
    }
    uint32_t offset = g_job_offset - g_job_chunk_len;
    errcode_t ret = send_job_data_chunk(offset, g_job_chunk, g_job_chunk_len);
    if (ret == ERRCODE_SUCC) {
        g_job_chunk_len = 0;
    }
    return ret;
}

static void handle_data_byte(uint8_t ch)
{
    if (!g_data_mode) {
        return;
    }
    if (g_job_offset >= g_job_total) {
        return;
    }

    g_job_chunk[g_job_chunk_len++] = ch;
    g_job_offset++;

    if (g_job_offset >= g_data_log_next || g_job_offset >= g_job_total) {
        osal_printk("[JOB_TX_DATA_RX] job=%u off=%u/%u\r\n",
                    (unsigned int)g_job_id, (unsigned int)g_job_offset,
                    (unsigned int)g_job_total);
        while (g_data_log_next <= g_job_offset) {
            g_data_log_next += TX_DATA_RX_LOG_STEP;
        }
    }

    if (g_job_chunk_len >= JOB_TX_DATA_CHUNK_MAX || g_job_offset >= g_job_total) {
        if (flush_job_chunk() != ERRCODE_SUCC) {
            osal_printk("[JOB_TX] chunk send fail off=%u\r\n", (unsigned int)g_job_offset);
            g_data_mode = false;
            return;
        }
        host_sendf("@ACK type=%u seq=0 status=%u offset=%u\r\n",
                   PKT_JOB_DATA, JOB_STATUS_OK, (unsigned int)g_job_offset);
    }

    if (g_job_offset >= g_job_total) {
        g_data_mode = false;
        osal_printk("[JOB_TX_DATA_COMPLETE] job=%u size=%u crc=0x%04x\r\n",
                    (unsigned int)g_job_id, (unsigned int)g_job_total, g_job_crc);
        if (send_job_end() == ERRCODE_SUCC) {
            osal_printk("[JOB_TX] job upload complete job=%u size=%u crc=0x%04x\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_total, g_job_crc);
            host_sendf("@JOB_READY job=%u size=%u\r\n", (unsigned int)g_job_id, (unsigned int)g_job_total);
        }
    }
}

static void send_simple_control(uint8_t type)
{
    (void)send_packet_wait_ack(type, NULL, 0);
}

static void handle_command_line(char *line)
{
    osal_printk("[JOB_TX_CMD] line=\"%s\" data_mode=%u off=%u/%u\r\n",
                line, (unsigned int)g_data_mode, (unsigned int)g_job_offset,
                (unsigned int)g_job_total);

    if (strncmp(line, "@BEGIN ", 7) == 0) {
        unsigned long job_id;
        unsigned long total;
        unsigned long crc;
        if (sscanf(line + 7, "%lu %lu %lx", &job_id, &total, &crc) != 3 ||
            total == 0) {
            host_sendf("@ERR bad_begin\r\n");
            return;
        }
        if (send_job_begin((uint32_t)job_id, (uint32_t)total, (uint16_t)crc) != ERRCODE_SUCC) {
            host_sendf("@ERR begin_failed\r\n");
            return;
        }
        g_job_id = (uint32_t)job_id;
        g_job_total = (uint32_t)total;
        g_job_crc = (uint16_t)crc;
        g_job_offset = 0;
        g_job_chunk_len = 0;
        g_data_log_next = TX_DATA_RX_LOG_STEP;
        g_data_mode = true;
        osal_printk("[JOB_TX_DATA_MODE] begin job=%u size=%u crc=0x%04x\r\n",
                    (unsigned int)g_job_id, (unsigned int)g_job_total, g_job_crc);
        host_sendf("@DATA_READY job=%u size=%u\r\n", (unsigned int)g_job_id, (unsigned int)g_job_total);
        return;
    }

    if (strncmp(line, "@EXEC_START ", 12) == 0) {
        exec_start_payload_t start = {0};
        start.job_id = (uint32_t)strtoul(line + 12, NULL, 0);
        (void)send_packet_wait_ack(PKT_EXEC_START, &start, sizeof(start));
        return;
    }
    if (strcmp(line, "@EXEC_STOP") == 0) {
        send_simple_control(PKT_EXEC_STOP);
        return;
    }
    if (strcmp(line, "@ABORT") == 0) {
        send_simple_control(PKT_JOB_ABORT);
        return;
    }
    if (strcmp(line, "@STATUS") == 0) {
        uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
        uint16_t packet_len = 0;
        uint16_t seq = next_seq();
        if (sle_packet_encode(PKT_STATUS_REQ, 0, seq, NULL, 0, packet, sizeof(packet), &packet_len)) {
            (void)sle_job_client_send_packet(packet, packet_len);
        }
        return;
    }

    host_sendf("@ERR unknown_command\r\n");
}

static errcode_t job_uart_init(void)
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
    errcode_t ret = uapi_uart_init(LASER_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_cfg);
    osal_printk("[JOB_TX] uart init ret=0x%x bus=%d baud=%d\r\n", ret, LASER_UART_BUS, UART_BAUD_RATE);
    return ret;
}

static int uart_rx_task(void *arg)
{
    unused(arg);
    uint8_t ch;
    uint32_t data_idle_ticks = 0;
    uint32_t idle_ticks = 0;
    while (1) {
        int32_t ret = uapi_uart_read(LASER_UART_BUS, &ch, 1, JOB_TX_UART_READ_TIMEOUT_MS);
        if (ret <= 0) {
            if (g_data_mode) {
                data_idle_ticks++;
                if ((data_idle_ticks % TX_UART_IDLE_LOG_TICKS) == 0U) {
                    osal_printk("[JOB_TX_UART_IDLE] data_mode=1 off=%u/%u status=%s\r\n",
                                (unsigned int)g_job_offset, (unsigned int)g_job_total,
                                sle_job_client_get_status());
                }
            } else {
                idle_ticks++;
                if ((idle_ticks % TX_UART_IDLE0_LOG_TICKS) == 0U) {
                    osal_printk("[JOB_TX_UART_IDLE] data_mode=0 line_len=%u off=%u/%u status=%s\r\n",
                                (unsigned int)g_line_len, (unsigned int)g_job_offset,
                                (unsigned int)g_job_total, sle_job_client_get_status());
                }
            }
            sle_job_client_poll_connect();
            osal_msleep(1);
            continue;
        }
        data_idle_ticks = 0;
        idle_ticks = 0;

        if (g_data_mode) {
            handle_data_byte(ch);
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (g_line_len > 0) {
                g_line[g_line_len] = '\0';
                handle_command_line(g_line);
                g_line_len = 0;
            }
        } else if (g_line_len < (TX_LINE_MAX - 1U)) {
            g_line[g_line_len++] = (char)ch;
        } else {
            osal_printk("[JOB_TX_CMD] line too long, reset parser\r\n");
            g_line_len = 0;
            host_sendf("@ERR line_too_long\r\n");
        }
    }
    return 0;
}

static void create_task(const char *name, osal_kthread_handler entry, uint32_t stack_size, uint32_t prio)
{
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(entry, NULL, name, stack_size);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[JOB_TX] create task %s failed\r\n", name);
        return;
    }
    if (osal_kthread_set_priority(task, prio) != OSAL_SUCCESS) {
        osal_printk("[JOB_TX] set task priority %s failed\r\n", name);
    }
    osal_kfree(task);
    osal_kthread_unlock();
}

static int sle_init_task(void *arg)
{
    unused(arg);

    osal_printk("[JOB_TX_BOOT] sle init task start, delay for stack ready\r\n");
    osal_msleep(500);

    osal_printk("[JOB_TX_BOOT] set response cb\r\n");
    sle_job_client_set_response_cb(response_cb);

    osal_printk("[JOB_TX_BOOT] sle client init begin\r\n");
    errcode_t ret = sle_job_client_init();
    osal_printk("[JOB_TX_BOOT] sle client init end ret=0x%x\r\n", ret);
    return (ret == ERRCODE_SUCC) ? 0 : -1;
}

static void laser_sle_job_tx_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser SLE Job TX\r\n");
    osal_printk("  mode: UART job input -> SLE packets\r\n");
    osal_printk("========================================\r\n");

    if (osal_sem_init(&g_ack_sem, 0) == OSAL_SUCCESS) {
        g_ack_sem_ready = true;
        osal_printk("[JOB_TX_BOOT] ack sem ready\r\n");
    } else {
        osal_printk("[JOB_TX_BOOT] ack sem init failed\r\n");
    }
    if (job_uart_init() != ERRCODE_SUCC) {
        return;
    }
    create_task("job_uart_rx", uart_rx_task, TASK_STACK_SIZE_DEFAULT, TASK_PRIO_JOB_UART);
    create_task("job_sle_init", sle_init_task, TASK_STACK_SIZE_SLE, TASK_PRIO_SLE);
    osal_printk("[JOB_TX_BOOT] tasks created\r\n");
}

app_run(laser_sle_job_tx_entry);
