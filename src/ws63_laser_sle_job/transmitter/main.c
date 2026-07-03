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
#include "systick.h"
#include "uart.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TX_LINE_MAX 96
#define TX_PAYLOAD_BUF_SIZE SLE_JOB_PACKET_MAX_PAYLOAD
#define TX_DATA_RX_LOG_STEP 32U
#define TX_DATA_MODE_TIMEOUT_TICKS 5000U
#define TX_FIRMWARE_PACKAGE "ws63-liteos-app_tx_all.fwpkg"

_Static_assert(sizeof(job_data_payload_t) + JOB_TX_DATA_CHUNK_MAX <= SLE_JOB_PACKET_MAX_PAYLOAD,
               "JOB_TX_DATA_CHUNK_MAX too large for SLE payload");

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
static uint32_t g_diag_data_count = 0;
static uint32_t g_wait_start_ms = 0;

static uint32_t g_job_id = 0;
static uint32_t g_job_total = 0;
static uint32_t g_job_offset = 0;
static uint16_t g_job_crc = 0;
static bool g_data_mode = false;
static uint32_t g_preroll_bytes = 0;
static bool g_preroll_signaled = false;
static uint8_t g_job_chunk[JOB_TX_DATA_CHUNK_MAX];
static uint16_t g_job_chunk_len = 0;
static uint32_t g_data_log_next = TX_DATA_RX_LOG_STEP;
static char g_line[TX_LINE_MAX];
static uint16_t g_line_len = 0;
static bool g_uart_control_frame_active = false;

static void clear_local_job_state(void)
{
    g_job_id = 0;
    g_job_total = 0;
    g_job_offset = 0;
    g_job_crc = 0;
    g_data_mode = false;
    g_preroll_bytes = 0;
    g_preroll_signaled = false;
    g_job_chunk_len = 0;
    g_data_log_next = TX_DATA_RX_LOG_STEP;
    g_diag_data_count = 0;
    g_line_len = 0;
    g_uart_control_frame_active = false;
}

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

static bool tx_should_log_timing(uint8_t type, uint32_t data_index, uint32_t total_ms)
{
#if JOB_TX_TIMING_LOG
    if (type != PKT_JOB_DATA) {
        return true;
    }
    return data_index <= JOB_TX_TIMING_FIRST_PACKETS ||
           (JOB_TX_TIMING_EVERY_PACKETS > 0U && (data_index % JOB_TX_TIMING_EVERY_PACKETS) == 0U) ||
           total_ms >= JOB_TX_TIMING_SLOW_MS;
#else
    unused(type);
    unused(data_index);
    unused(total_ms);
    return false;
#endif
}

static errcode_t send_packet_wait_ack(uint8_t type, const void *payload, uint16_t payload_len)
{
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = next_seq();
    uint16_t actual_payload_len = payload_len_for_type(type, payload, payload_len);
    uint32_t dbg_off = 0;
    uint16_t dbg_dlen = 0;
    uint32_t dbg_data_index = 0;

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

    {
        if (type == PKT_JOB_DATA && payload != NULL && payload_len >= sizeof(job_data_payload_t)) {
            const job_data_payload_t *dp = (const job_data_payload_t *)payload;
            dbg_off = dp->offset;
            dbg_dlen = dp->data_len;
            g_diag_data_count++;
            dbg_data_index = g_diag_data_count;
        }
        if (JOB_DIAG_LOG && g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA) {
            osal_printk("[TX_SEND] t=%u seq=%u type=0x%02x off=%u dlen=%u plen=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq, type,
                        (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                        (unsigned int)actual_payload_len);
        }
    }

    if (JOB_DIAG_LOG) {
        osal_printk("[JOB_TX_SEND_ENTER] type=0x%02x seq=%u payload=%u packet=%u conn=%u status=%s\r\n",
                    type, seq, actual_payload_len, packet_len,
                    (unsigned int)sle_job_client_is_connected(), sle_job_client_get_status());
    }

    for (uint32_t retry = 0; retry <= JOB_TX_RETRY_MAX; retry++) {
        uint32_t reconnect_start = (uint32_t)uapi_systick_get_ms();
        while (!sle_job_client_is_connected()) {
            uint32_t reconnect_elapsed =
                (uint32_t)uapi_systick_get_ms() - reconnect_start;
            if (reconnect_elapsed >= JOB_TX_CONNECT_WAIT_MS) {
                osal_printk("[JOB_TX_WAIT_CONN_TIMEOUT] type=0x%02x seq=%u waited=%u status=%s\r\n",
                            type, seq, (unsigned int)reconnect_elapsed,
                            sle_job_client_get_status());
                g_wait_active = false;
                g_wait_ack_seq = 0;
                host_sendf("@NACK type=%u seq=%u status=%u reason=no_link\r\n",
                           type, seq, JOB_STATUS_NOT_READY);
                return ERRCODE_FAIL;
            }
            sle_job_client_poll_connect();
            osal_msleep(20);
        }

        if (g_wait_got_ack && g_wait_status == JOB_STATUS_OK) {
            g_wait_active = false;
            g_wait_ack_seq = 0;
            return ERRCODE_SUCC;
        }

        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_SEND_CALL] type=0x%02x seq=%u try=%u packet=%u status=%s\r\n",
                        type, seq, (unsigned int)retry, packet_len, sle_job_client_get_status());
        }
        uint32_t t_send = (uint32_t)uapi_systick_get_ms();
        errcode_t ret = sle_job_client_send_packet(packet, packet_len);
        uint32_t send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
        if (JOB_DIAG_LOG && (g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA || ret != ERRCODE_SLE_SUCCESS)) {
            osal_printk("[TX_CFM] t=%u seq=%u ret=0x%x cost=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)ret,
                        (unsigned int)send_ms);
        }
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_FRAME] type=0x%02x seq=%u len=%u try=%u ret=0x%x\r\n",
                        type, seq, packet_len, (unsigned int)retry, ret);
        }
        if (ret == ERRCODE_SLE_SUCCESS) {
            uint32_t t_ack = (uint32_t)uapi_systick_get_ms();
            g_wait_start_ms = t_ack;
            if (osal_sem_down_timeout(&g_ack_sem, JOB_TX_ACK_TIMEOUT_MS) == OSAL_SUCCESS &&
                g_wait_got_ack) {
                uint32_t ack_ms = (uint32_t)uapi_systick_get_ms() - t_ack;
                uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_send;
                if (tx_should_log_timing(type, dbg_data_index, total_ms)) {
                    osal_printk("[TX_TIMING] type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                                "send_ms=%u ack_ms=%u total_ms=%u retry=%u status=%u\r\n",
                                type, seq, (unsigned int)dbg_data_index,
                                (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                                (unsigned int)send_ms, (unsigned int)ack_ms,
                                (unsigned int)total_ms, (unsigned int)retry,
                                (unsigned int)g_wait_status);
                }
                if (g_wait_status == JOB_STATUS_OK) {
                    g_wait_active = false;
                    g_wait_ack_seq = 0;
                    return ERRCODE_SUCC;
                }
                break;
            }
            osal_printk("[TX_TO] t=%u seq=%u retry=%u waited=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)retry,
                        (unsigned int)((uint32_t)uapi_systick_get_ms() - t_ack));
        }
    }

    g_wait_active = false;
    g_wait_ack_seq = 0;
    host_sendf("@NACK type=%u seq=%u status=%u\r\n", type, seq, g_wait_status);
    return ERRCODE_FAIL;
}

static errcode_t abort_rx_and_clear_transaction(const char *reason)
{
    uint32_t old_job = g_job_id;
    uint32_t old_offset = g_job_offset;
    uint32_t old_total = g_job_total;
    bool was_data_mode = g_data_mode;

    /* Stop accepting bytes for the stale transaction before waiting on SLE. */
    clear_local_job_state();
    unused(reason);
    unused(old_job);
    unused(old_offset);
    unused(old_total);
    unused(was_data_mode);

    if (!sle_job_client_is_connected()) {
        osal_printk("[JOB_TX_RESYNC] RX abort not confirmed: SLE disconnected\r\n");
        return ERRCODE_FAIL;
    }

    errcode_t ret = send_packet_wait_ack(PKT_JOB_ABORT, NULL, 0);
    return ret;
}

static void handle_uart_resync(void)
{
    if (abort_rx_and_clear_transaction("uart-can") == ERRCODE_SUCC) {
        host_sendf("@OK resync rx=aborted\r\n");
    } else {
        host_sendf("@ERR resync_failed rx=unconfirmed\r\n");
    }
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
        if (!g_wait_active || ack.ack_seq != g_wait_ack_seq) {
            osal_printk("[TX_OLD] t=%u ack_seq=%u wait=%u active=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), ack.ack_seq,
                        (unsigned int)g_wait_ack_seq, (unsigned int)g_wait_active);
            return;
        }
        if (JOB_DIAG_LOG && (g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA || ack.status != 0)) {
            osal_printk("[TX_ACK] t=%u ack_seq=%u wait=%u st=%u off=%u active=%u cost=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), ack.ack_seq,
                        (unsigned int)g_wait_ack_seq, ack.status, (unsigned int)ack.offset,
                        (unsigned int)g_wait_active,
                        (unsigned int)(uapi_systick_get_ms() - g_wait_start_ms));
        }
        g_wait_status = ack.status;
        g_wait_got_ack = true;
        if (g_ack_sem_ready) {
            osal_sem_up(&g_ack_sem);
        }
        return;
    }

    if (pkt.type == PKT_STATUS_RESP && pkt.len == sizeof(status_resp_payload_t)) {
        status_resp_payload_t st;
        memcpy(&st, pkt.payload, sizeof(st));
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_STATUS] state=%u status=%u job=%u rx=%u/%u free=%u lines=%u\r\n",
                        st.state, st.status, (unsigned int)st.job_id,
                        (unsigned int)st.received_size, (unsigned int)st.total_size,
                        (unsigned int)st.cache_free, (unsigned int)st.executed_lines);
        }
        host_sendf("@STATUS state=%u status=%u job=%u rx=%u total=%u free=%u lines=%u\r\n",
                   st.state, st.status, (unsigned int)st.job_id,
                   (unsigned int)st.received_size, (unsigned int)st.total_size,
                   (unsigned int)st.cache_free, (unsigned int)st.executed_lines);
        if (sle_job_client_panel_is_connected()) {
            (void)sle_job_client_mirror_panel_packet(data, length);
        }
        return;
    }

    if (pkt.type == PKT_PANEL_STATUS && pkt.len == sizeof(panel_status_payload_t)) {
        panel_status_payload_t st;
        memcpy(&st, pkt.payload, sizeof(st));
        if (JOB_DIAG_LOG) {
            osal_printk("[PANEL_STATUS] seq=%u owner=%u mode=%u state=%u flags=0x%02x job=%u rx=%u/%u lines=%u free=%u err=%u tick=%u\r\n",
                        (unsigned int)st.seq, st.owner, st.mode, st.job_state, st.flags,
                        (unsigned int)st.job_id, (unsigned int)st.received_size,
                        (unsigned int)st.total_size, (unsigned int)st.executed_lines,
                        (unsigned int)st.cache_free, (unsigned int)st.last_error,
                        (unsigned int)st.tick_ms);
        }
        if (sle_job_client_panel_is_connected()) {
            (void)sle_job_client_mirror_panel_packet(data, length);
        }
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
    if (JOB_DIAG_LOG && ret == ERRCODE_SUCC) {
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
    uint16_t len = g_job_chunk_len;
    errcode_t ret = send_job_data_chunk(offset, g_job_chunk, g_job_chunk_len);
    if (ret == ERRCODE_SUCC) {
        g_job_chunk_len = 0;
    }
    if (JOB_DIAG_LOG && (g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA || ret != ERRCODE_SUCC)) {
        osal_printk("[TX_CHUNK_DONE] t=%u off=%u len=%u ret=0x%x\r\n",
                    (unsigned int)uapi_systick_get_ms(), (unsigned int)offset,
                    (unsigned int)len, (unsigned int)ret);
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

    if (ch == '@' && g_preroll_bytes > 0 && !g_preroll_signaled) {
        osal_printk("[JOB_TX_DATA_SUSPICIOUS_AT] off=%u/%u chunk_len=%u preroll_signaled=%d\r\n",
                    (unsigned int)g_job_offset, (unsigned int)g_job_total,
                    (unsigned int)g_job_chunk_len, (int)g_preroll_signaled);
    }

    g_job_chunk[g_job_chunk_len++] = ch;
    g_job_offset++;

    if (g_job_offset >= g_data_log_next || g_job_offset >= g_job_total) {
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_DATA_RX] job=%u off=%u/%u\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_offset,
                        (unsigned int)g_job_total);
        }
        while (g_data_log_next <= g_job_offset) {
            g_data_log_next += TX_DATA_RX_LOG_STEP;
        }
    }

    if (g_preroll_bytes > 0 && !g_preroll_signaled &&
        g_job_offset >= g_preroll_bytes && g_job_offset < g_job_total) {
        if (flush_job_chunk() != ERRCODE_SUCC) {
            osal_printk("[JOB_TX] preroll chunk send fail off=%u\r\n", (unsigned int)g_job_offset);
            (void)abort_rx_and_clear_transaction("preroll-chunk-send-fail");
            host_sendf("@ERR chunk_send_fail_abort\r\n");
            return;
        }
        g_preroll_signaled = true;
        g_data_mode = false;
        host_sendf("@ACK type=%u seq=0 status=%u offset=%u preroll=1\r\n",
                   PKT_JOB_DATA, JOB_STATUS_OK, (unsigned int)g_job_offset);
        return;
    }

    if (g_job_chunk_len >= JOB_TX_DATA_CHUNK_MAX || g_job_offset >= g_job_total) {
        if (flush_job_chunk() != ERRCODE_SUCC) {
            osal_printk("[JOB_TX] chunk send fail off=%u\r\n", (unsigned int)g_job_offset);
            (void)abort_rx_and_clear_transaction("chunk-send-fail");
            host_sendf("@ERR chunk_send_fail_abort\r\n");
            return;
        }
        host_sendf("@ACK type=%u seq=0 status=%u offset=%u\r\n",
                   PKT_JOB_DATA, JOB_STATUS_OK, (unsigned int)g_job_offset);
    }

    if (g_job_offset >= g_job_total) {
        g_data_mode = false;
        if (send_job_end() == ERRCODE_SUCC) {
            osal_printk("[JOB_TX] job upload complete job=%u size=%u crc=0x%04x\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_total, g_job_crc);
            host_sendf("@JOB_READY job=%u size=%u\r\n", (unsigned int)g_job_id, (unsigned int)g_job_total);
        } else {
            (void)abort_rx_and_clear_transaction("job-end-fail");
            host_sendf("@ERR job_end_failed_abort\r\n");
        }
    }
}

static errcode_t send_simple_control(uint8_t type)
{
    errcode_t ret = send_packet_wait_ack(type, NULL, 0);
    if (ret == ERRCODE_SUCC) {
        host_sendf("@ACK type=%u seq=0 status=0\r\n", (unsigned int)type);
    }
    return ret;
}

static void send_focus_off_control(void)
{
    focus_ctrl_payload_t fp = {0};
    fp.on = 0;
    fp.power = 0;
    if (send_packet_wait_ack(PKT_FOCUS_CTRL, &fp, sizeof(fp)) == ERRCODE_SUCC) {
        host_sendf("@OK focus_off\r\n");
    } else {
        host_sendf("@NACK focus_reject\r\n");
    }
}

static void handle_uart_control_frame(uint8_t code)
{
    switch (code) {
        case JOB_TX_UART_CONTROL_EXEC_STOP:
            (void)send_simple_control(PKT_EXEC_STOP);
            return;
        case JOB_TX_UART_CONTROL_EXEC_RESUME:
            (void)send_simple_control(PKT_EXEC_RESUME);
            return;
        case JOB_TX_UART_CONTROL_ABORT:
            if (abort_rx_and_clear_transaction("uart-control-abort") == ERRCODE_SUCC) {
                host_sendf("@ACK type=%u seq=0 status=0\r\n", (unsigned int)PKT_JOB_ABORT);
            } else {
                host_sendf("@ERR abort_failed rx=unconfirmed\r\n");
            }
            return;
        case JOB_TX_UART_CONTROL_FOCUS_OFF:
            send_focus_off_control();
            return;
        default:
            osal_printk("[JOB_TX_CTRL_FRAME] unsupported code=0x%02x\r\n", code);
            host_sendf("@ERR bad_control_frame\r\n");
            return;
    }
}

static void send_route_switch_to_wifi(void)
{
    route_switch_payload_t payload = {0};
    payload.target_route = SLE_JOB_ROUTE_TARGET_LEGACY_WIFI;
    payload.flags = 0;
    payload.reserved = 0;

    host_sendf("@OK route_switch_request target=LEGACY_WIFI\r\n");
    osal_printk("[JOB_TX_ROUTE_SWITCH] target=LEGACY_WIFI\r\n");
    if (send_packet_wait_ack(PKT_ROUTE_SWITCH, &payload, sizeof(payload)) == ERRCODE_SUCC) {
        host_sendf("@OK route_switch_accepted target=LEGACY_WIFI\r\n");
    } else {
        host_sendf("@ERR route_switch_failed status=%u\r\n", (unsigned int)g_wait_status);
    }
}

static void handle_command_line(char *line)
{
    if (JOB_DIAG_LOG) {
        osal_printk("[JOB_TX_CMD] line=\"%s\" data_mode=%d off=%u/%u preroll_bytes=%u preroll_signaled=%d\r\n",
                    line, (int)g_data_mode, (unsigned int)g_job_offset,
                    (unsigned int)g_job_total, (unsigned int)g_preroll_bytes,
                    (int)g_preroll_signaled);
    }

    if (strncmp(line, "@BEGIN ", 7) == 0) {
        unsigned long job_id;
        unsigned long total;
        unsigned long crc;
        unsigned long preroll = 0;
        const char *tag = " preroll=";
        const char *preroll_ptr = strstr(line, tag);
        int parsed = sscanf(line + 7, "%lu %lu %lx", &job_id, &total, &crc);
        if (parsed == 3 && preroll_ptr != NULL) {
            preroll = strtoul(preroll_ptr + strlen(tag), NULL, 10);
        }
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_BEGIN_PARSE] raw=\"%s\" job=%u total=%u crc=0x%04x preroll=%u found_preroll=%d\r\n",
                        line, (unsigned int)job_id, (unsigned int)total, (unsigned int)crc,
                        (unsigned int)preroll, (int)(preroll_ptr != NULL));
        }
        if (parsed != 3 || total == 0) {
            host_sendf("@ERR bad_begin\r\n");
            return;
        }
        if (send_job_begin((uint32_t)job_id, (uint32_t)total, (uint16_t)crc) != ERRCODE_SUCC) {
            (void)abort_rx_and_clear_transaction("job-begin-fail");
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
        g_diag_data_count = 0;
        g_preroll_bytes = (uint32_t)preroll;
        g_preroll_signaled = false;
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_DATA_MODE] begin job=%u size=%u crc=0x%04x preroll=%u\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_total, g_job_crc,
                        (unsigned int)g_preroll_bytes);
        }
        host_sendf("@DATA_READY job=%u size=%u\r\n", (unsigned int)g_job_id, (unsigned int)g_job_total);
        return;
    }

    if (strncmp(line, "@EXEC_START ", 12) == 0) {
        exec_start_payload_t start = {0};
        start.job_id = (uint32_t)strtoul(line + 12, NULL, 0);
        if (send_packet_wait_ack(PKT_EXEC_START, &start, sizeof(start)) == ERRCODE_SUCC) {
            host_sendf("@ACK type=16 seq=0 status=0\r\n");
        }
        return;
    }
    if (strcmp(line, "@DATA_RESUME") == 0) {
        if (g_preroll_signaled && g_job_offset < g_job_total && !g_data_mode) {
            g_data_mode = true;
            host_sendf("@OK data_resume\r\n");
        } else {
            osal_printk("[JOB_TX_DATA_RESUME] fail preroll_signaled=%d off=%u/%u data_mode=%d\r\n",
                        (int)g_preroll_signaled, (unsigned int)g_job_offset,
                        (unsigned int)g_job_total, (int)g_data_mode);
            host_sendf("@ERR cannot_resume\r\n");
        }
        return;
    }
    if (strcmp(line, "@EXEC_STOP") == 0) {
        send_simple_control(PKT_EXEC_STOP);
        return;
    }
    if (strcmp(line, "@EXEC_RESUME") == 0) {
        send_simple_control(PKT_EXEC_RESUME);
        return;
    }
    if (strcmp(line, "@ABORT") == 0) {
        if (abort_rx_and_clear_transaction("host-abort") == ERRCODE_SUCC) {
            host_sendf("@ACK type=%u seq=0 status=0\r\n", (unsigned int)PKT_JOB_ABORT);
        } else {
            host_sendf("@ERR abort_failed rx=unconfirmed\r\n");
        }
        return;
    }
    if (strcmp(line, "@RESET") == 0) {
        if (abort_rx_and_clear_transaction("host-reset") == ERRCODE_SUCC) {
            host_sendf("@OK reset rx=aborted\r\n");
        } else {
            host_sendf("@ERR reset_failed rx=unconfirmed\r\n");
        }
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
    if (strcmp(line, "@RX MODE=GRBL") == 0) {
        if (g_data_mode) {
            host_sendf("@ERR route_switch_busy data_mode=1\r\n");
            return;
        }
        send_route_switch_to_wifi();
        return;
    }

    if (strncmp(line, "@FOCUS_ON S", 11) == 0) {
        unsigned long s = strtoul(line + 11, NULL, 10);
        if (s > 100) {
            host_sendf("@ERR focus_bad_power\r\n");
            return;
        }
        focus_ctrl_payload_t fp = {0};
        fp.on = 1;
        fp.power = (uint8_t)s;
        osal_printk("[FOCUS_TX] on s=%u sizeof=%u\r\n", (unsigned int)s, (unsigned int)sizeof(fp));
        if (send_packet_wait_ack(PKT_FOCUS_CTRL, &fp, sizeof(fp)) == ERRCODE_SUCC) {
            host_sendf("@OK focus_on s=%u\r\n", (unsigned int)s);
        } else {
            host_sendf("@NACK focus_reject\r\n");
        }
        return;
    }
    if (strcmp(line, "@FOCUS_OFF") == 0) {
        send_focus_off_control();
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
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_TX] uart init failed: 0x%x\r\n", ret);
    }
    return ret;
}

static int uart_rx_task(void *arg)
{
    unused(arg);
    uint8_t ch;
    uint32_t data_idle_ticks = 0;
    while (1) {
        int32_t ret = uapi_uart_read(LASER_UART_BUS, &ch, 1, JOB_TX_UART_READ_TIMEOUT_MS);
        if (ret <= 0) {
            if (g_data_mode) {
                data_idle_ticks++;
                if (data_idle_ticks >= TX_DATA_MODE_TIMEOUT_TICKS) {
                    errcode_t reset_ret = abort_rx_and_clear_transaction("data-mode-timeout");
                    data_idle_ticks = 0;
                    host_sendf("@ERR data_mode_timeout recovery=%s\r\n",
                               (reset_ret == ERRCODE_SUCC) ? "safe" : "unconfirmed");
                }
            }
            sle_job_client_poll_connect();
            osal_msleep(1);
            continue;
        }
        data_idle_ticks = 0;

        /* ASCII CAN is an out-of-band transaction reset in every parser mode. */
        if (ch == JOB_TX_UART_RESYNC_BYTE) {
            handle_uart_resync();
            continue;
        }

        if (g_uart_control_frame_active) {
            g_uart_control_frame_active = false;
            handle_uart_control_frame(ch);
            continue;
        }

        if (ch == JOB_TX_UART_CONTROL_BYTE) {
            g_uart_control_frame_active = true;
            continue;
        }

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

    osal_msleep(500);

    sle_job_client_set_response_cb(response_cb);

    errcode_t ret = sle_job_client_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_TX_BOOT] sle client init failed: 0x%x\r\n", ret);
    }
    return (ret == ERRCODE_SUCC) ? 0 : -1;
}

static void laser_sle_job_tx_entry(void)
{
    osal_printk("[FW_ID] board=TX firmware=%s app=ws63_laser_sle_job/transmitter role=host-uart-to-sle payload=%u uart=%u\r\n",
                TX_FIRMWARE_PACKAGE,
                (unsigned int)JOB_TX_DATA_CHUNK_MAX,
                (unsigned int)UART_BAUD_RATE);
    if (osal_sem_init(&g_ack_sem, 0) == OSAL_SUCCESS) {
        g_ack_sem_ready = true;
    } else {
        osal_printk("[JOB_TX_BOOT] ack sem init failed\r\n");
    }
    if (job_uart_init() != ERRCODE_SUCC) {
        return;
    }
    host_sendf("@FW board=TX firmware=%s app=ws63_laser_sle_job/transmitter role=host-uart-to-sle payload=%u uart=%u\r\n",
               TX_FIRMWARE_PACKAGE,
               (unsigned int)JOB_TX_DATA_CHUNK_MAX,
               (unsigned int)UART_BAUD_RATE);
    create_task("job_uart_rx", uart_rx_task, TASK_STACK_SIZE_DEFAULT, TASK_PRIO_JOB_UART);
    create_task("job_sle_init", sle_init_task, TASK_STACK_SIZE_SLE, TASK_PRIO_SLE);
}

app_run(laser_sle_job_tx_entry);
