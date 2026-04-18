/**
 * @file wifi_gcode_server.c
 * @brief 发射板 WiFi G-Code 入口
 *        参考官方 SoftAP / STA 示例，在不动 UART / SLE 主链路的前提下，
 *        为发射板补充更完整的 WiFi 状态管理、模式选择和 TCP 异常恢复能力。
 */
#include "wifi_gcode_server.h"

#include "config.h"
#include "protocol.h"
#include "gcode_processor.h"
#include "sle_client.h"
#include "sle_errcode.h"
#include "soc_osal.h"
#include "lwip/netifapi.h"
#include "lwip/sockets.h"
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "wifi_device.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define WIFI_GCODE_LOG "[wifi gcode]"
#define WIFI_GCODE_RX_LINE_MAX 128
#define WIFI_GCODE_RX_BUF_SIZE 256
/*
 * 仍然只允许一个有效上游会话，但把 backlog 略微放大，避免短时间重连/多端探测时，
 * 连接在来得及被 accept+busy 拒绝前就被 TCP 栈直接丢掉。
 */
#define WIFI_GCODE_LISTEN_BACKLOG 4
#define WIFI_GCODE_RETRY_DELAY_MS 1000
#define WIFI_GCODE_STA_SCAN_AP_LIMIT 32
#define WIFI_GCODE_POLL_SLICE_MS 100
#define WIFI_GCODE_STATUS_RESP_MAX 320

static wifi_gcode_status_snapshot_t g_wifi_status;
static volatile bool g_wifi_scan_done = false;

static void wifi_status_clear_ip(void)
{
    (void)snprintf(g_wifi_status.ip, sizeof(g_wifi_status.ip), "0.0.0.0");
}

static const char *wifi_mode_name(wifi_gcode_mode_t mode)
{
    return (mode == WIFI_GCODE_MODE_STA) ? "STA" : "SoftAP";
}

static void wifi_event_scan_state_changed(int32_t state, int32_t size);
static void wifi_event_connection_changed(int32_t state, const wifi_linked_info_stru *info, int32_t reason_code);
static void wifi_event_softap_state_changed(int32_t state);
static void wifi_event_softap_sta_join(const wifi_sta_info_stru *info);
static void wifi_event_softap_sta_leave(const wifi_sta_info_stru *info);

static wifi_event_stru g_wifi_event_cb = {
    .wifi_event_connection_changed = wifi_event_connection_changed,
    .wifi_event_scan_state_changed = wifi_event_scan_state_changed,
    .wifi_event_softap_state_changed = wifi_event_softap_state_changed,
    .wifi_event_softap_sta_join = wifi_event_softap_sta_join,
    .wifi_event_softap_sta_leave = wifi_event_softap_sta_leave,
};

static bool wifi_netif_has_ipv4(const char *ifname)
{
    struct netif *netif_p = netif_find(ifname);
    if (netif_p == NULL) {
        return false;
    }
    return (ip_addr_isany(&(netif_p->ip_addr)) == 0);
}

static void wifi_status_refresh_ip(void)
{
    struct netif *netif_p = netif_find(g_wifi_status.ifname);
    uint32_t addr;

    if ((netif_p == NULL) || (ip_addr_isany(&(netif_p->ip_addr)) != 0)) {
        wifi_status_clear_ip();
        return;
    }

    addr = netif_p->ip_addr.u_addr.ip4.addr;
    (void)snprintf(g_wifi_status.ip, sizeof(g_wifi_status.ip), "%u.%u.%u.%u", addr & 0xFFU, (addr >> 8) & 0xFFU,
                   (addr >> 16) & 0xFFU, (addr >> 24) & 0xFFU);
}

static void wifi_status_reset_runtime(void)
{
    g_wifi_status.net_ready = false;
    g_wifi_status.tcp_listening = false;
    g_wifi_status.client_connected = false;
    g_wifi_status.sta_link_up = false;
    g_wifi_status.softap_sta_count = 0;
    g_wifi_status.last_disconnect_reason = 0;
    g_wifi_scan_done = false;
    wifi_status_clear_ip();
}

static void wifi_status_init_defaults(void)
{
    memset(&g_wifi_status, 0, sizeof(g_wifi_status));
    g_wifi_status.mode = LASER_WIFI_STA_MODE_ENABLED ? WIFI_GCODE_MODE_STA : WIFI_GCODE_MODE_SOFTAP;
    g_wifi_status.tcp_port = (uint16_t)LASER_WIFI_TCP_PORT;
    (void)snprintf(g_wifi_status.ifname, sizeof(g_wifi_status.ifname), "%s",
                   LASER_WIFI_STA_MODE_ENABLED ? LASER_WIFI_STA_IFNAME : LASER_WIFI_SOFTAP_IFNAME);
    (void)snprintf(g_wifi_status.ssid, sizeof(g_wifi_status.ssid), "%s",
                   LASER_WIFI_STA_MODE_ENABLED ? LASER_WIFI_STA_SSID : LASER_WIFI_SOFTAP_SSID);
    wifi_status_clear_ip();
}

void wifi_gcode_server_get_status(wifi_gcode_status_snapshot_t *status)
{
    if (status == NULL) {
        return;
    }
    wifi_status_refresh_ip();
    memcpy(status, &g_wifi_status, sizeof(*status));
}

static bool wifi_is_network_ready(void)
{
    if (g_wifi_status.mode == WIFI_GCODE_MODE_STA) {
        return g_wifi_status.sta_link_up && g_wifi_status.net_ready && wifi_netif_has_ipv4(g_wifi_status.ifname);
    }
    return g_wifi_status.net_ready && (netif_find(g_wifi_status.ifname) != NULL);
}

static void wifi_send_log(int client_sock, const char *msg)
{
    size_t left = strlen(msg);
    const char *ptr = msg;

    while (left > 0) {
        int sent = send(client_sock, ptr, left, 0);
        if (sent <= 0) {
            return;
        }
        ptr += (size_t)sent;
        left -= (size_t)sent;
    }
}

static void wifi_log_link_state(const char *reason, const motion_cmd_t *cmd)
{
    osal_printk(WIFI_GCODE_LOG
                " %s: cmd=0x%x conn=%u handles=%u status_rx=%u cmd_hdl=0x%x status_hdl=0x%x pending=%u queue_free=%u "
                "ack=%u\r\n",
                reason, (cmd != NULL) ? cmd->cmd : 0, sle_laser_client_is_connected() ? 1U : 0U,
                sle_laser_client_has_handles_ready() ? 1U : 0U, sle_laser_client_has_status_rx() ? 1U : 0U,
                sle_laser_client_get_cmd_handle(), sle_laser_client_get_status_handle(),
                sle_laser_client_get_pending_writes(), sle_laser_client_get_queue_free(),
                sle_laser_client_get_last_ack_seq());
}

static bool wifi_send_motion_cmd_with_retry(const motion_cmd_t *cmd)
{
    uint32_t waited_ms = 0;

    while (sle_laser_client_is_ready()) {
        errcode_t ret = sle_laser_client_send_cmd(cmd);
        if (ret == ERRCODE_SUCC) {
            return true;
        }
        if (ret != ERRCODE_SLE_BUSY) {
            osal_printk(WIFI_GCODE_LOG " sle send fail: cmd=0x%x ret=0x%x\r\n", cmd->cmd, ret);
            wifi_log_link_state("sle send fail state", cmd);
            return false;
        }

        if (waited_ms >= SLE_TX_BUSY_RETRY_TIMEOUT_MS) {
            osal_printk(WIFI_GCODE_LOG " sle busy timeout: cmd=0x%x waited=%ums\r\n", cmd->cmd, waited_ms);
            wifi_log_link_state("sle busy timeout state", cmd);
            return false;
        }

        osal_msleep(SLE_TX_BUSY_RETRY_INTERVAL_MS);
        waited_ms += SLE_TX_BUSY_RETRY_INTERVAL_MS;
    }

    return false;
}

static bool wifi_seq_reached(uint16_t ack_seq, uint16_t target_seq)
{
    return (uint16_t)(ack_seq - target_seq) < 0x8000U;
}

static bool wifi_wait_cmd_ack(uint16_t seq)
{
    uint32_t waited_ms = 0;

    while (sle_laser_client_is_ready()) {
        if (wifi_seq_reached(sle_laser_client_get_last_ack_seq(), seq)) {
            return true;
        }
        if (waited_ms >= CMD_ACK_TIMEOUT_MS) {
            osal_printk(WIFI_GCODE_LOG " ack timeout: seq=%u last_ack=%u\r\n", seq,
                        sle_laser_client_get_last_ack_seq());
            return false;
        }
        osal_msleep(SLE_TX_BUSY_RETRY_INTERVAL_MS);
        waited_ms += SLE_TX_BUSY_RETRY_INTERVAL_MS;
    }

    return false;
}

static bool wifi_send_business_cmd_reliably(const motion_cmd_t *cmd)
{
    for (uint32_t attempt = 0; attempt <= CMD_RETRY_MAX; attempt++) {
        if (!wifi_send_motion_cmd_with_retry(cmd)) {
            return false;
        }
        if (wifi_wait_cmd_ack(cmd->seq)) {
            return true;
        }
        osal_printk(WIFI_GCODE_LOG " retry cmd seq=%u cmd=0x%x attempt=%u/%u\r\n", cmd->seq, cmd->cmd, attempt + 1,
                    CMD_RETRY_MAX + 1);
    }

    wifi_log_link_state("ack wait timeout", cmd);
    return false;
}

static void wifi_format_status_snapshot(char *buf, size_t buf_size)
{
    wifi_gcode_status_snapshot_t snapshot;

    wifi_gcode_server_get_status(&snapshot);
    (void)snprintf(buf, buf_size,
                   "[WIFI:MODE=%s,SSID=%s,IF=%s,IP=%s,NET=%u,TCP=%u,CLI=%u,APSTA=%u,STALINK=%u,SLE=%u,REASON=%d]\r\n"
                   "ok\r\n",
                   wifi_mode_name(snapshot.mode), snapshot.ssid, snapshot.ifname, snapshot.ip,
                   snapshot.net_ready ? 1U : 0U, snapshot.tcp_listening ? 1U : 0U, snapshot.client_connected ? 1U : 0U,
                   snapshot.softap_sta_count, snapshot.sta_link_up ? 1U : 0U, sle_laser_client_is_ready() ? 1U : 0U,
                   snapshot.last_disconnect_reason);
}

static bool wifi_is_status_query(const char *line)
{
    return (strcmp(line, "$WIFI") == 0) || (strcmp(line, "$WIFI?") == 0);
}

static void wifi_process_line(int client_sock, const char *line, int len)
{
    if (len == 0) {
        return;
    }

    if (line[0] == '?') {
        char buf[80];
        double cur_x = 0.0;
        double cur_y = 0.0;
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
        wifi_send_log(client_sock, buf);
        return;
    }

    if (wifi_is_status_query(line)) {
        char response[WIFI_GCODE_STATUS_RESP_MAX];
        wifi_format_status_snapshot(response, sizeof(response));
        wifi_send_log(client_sock, response);
        return;
    }

    {
        char response[128];
        if (grbl_process_dollar(line, response, sizeof(response))) {
            wifi_send_log(client_sock, response);
            return;
        }
    }

    {
        motion_cmd_t cmds[4];
        int cmd_count = 0;

        if (gcode_process_line(line, len, cmds, 4, &cmd_count)) {
            bool send_ok = true;

            for (int i = 0; i < cmd_count; i++) {
                if (!sle_laser_client_is_ready()) {
                    wifi_log_link_state("reject cmd not ready", &cmds[i]);
                    send_ok = false;
                    break;
                }

                {
                    int retry = 0;
                    while (sle_laser_client_is_ready() &&
                           sle_laser_client_get_queue_free() < FLOW_CTRL_PAUSE_THRESHOLD) {
                        osal_msleep(5);
                        retry++;
                        if (retry > 200) {
                            wifi_log_link_state("queue_free wait timeout", &cmds[i]);
                            send_ok = false;
                            break;
                        }
                    }
                }

                if (!send_ok) {
                    break;
                }

                if (!wifi_send_business_cmd_reliably(&cmds[i])) {
                    send_ok = false;
                    break;
                }
            }

            if (!send_ok) {
                wifi_send_log(client_sock, "error:2\r\n");
                return;
            }
        }
    }

    wifi_send_log(client_sock, "ok\r\n");
}

static void wifi_process_rx_stream(int client_sock,
                                   const uint8_t *buf,
                                   int len,
                                   char *line_buf,
                                   int *line_pos,
                                   bool *line_overflow)
{
    for (int i = 0; i < len; i++) {
        uint8_t ch = buf[i];

        if (ch == '\n' || ch == '\r') {
            if (*line_overflow) {
                wifi_send_log(client_sock, "error:line_too_long\r\n");
                *line_pos = 0;
                *line_overflow = false;
                continue;
            }
            if (*line_pos > 0) {
                line_buf[*line_pos] = '\0';
                wifi_process_line(client_sock, line_buf, *line_pos);
                *line_pos = 0;
            }
            continue;
        }

        if (*line_overflow) {
            continue;
        }

        if (*line_pos < (WIFI_GCODE_RX_LINE_MAX - 1)) {
            line_buf[*line_pos] = (char)ch;
            (*line_pos)++;
        } else {
            *line_overflow = true;
        }
    }
}

static uint16_t wifi_htons(uint16_t value)
{
    return (uint16_t)(((value & 0x00FFU) << 8) | ((value & 0xFF00U) >> 8));
}

static int wifi_wait_socket_readable(int sock, uint32_t timeout_ms)
{
    fd_set read_fds;
    struct timeval tv;

    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);
    tv.tv_sec = (long)(timeout_ms / 1000U);
    tv.tv_usec = (long)((timeout_ms % 1000U) * 1000U);
    return select(sock + 1, &read_fds, NULL, NULL, &tv);
}

static void wifi_wait_until_inited(void)
{
    uint32_t wait_ms = 0;

    while (wifi_is_wifi_inited() == 0) {
        if ((wait_ms % 1000U) == 0U) {
            osal_printk(WIFI_GCODE_LOG " waiting wifi init...\r\n");
        }
        osal_msleep(10);
        wait_ms += 10U;
    }

    osal_printk(WIFI_GCODE_LOG " wifi init ready\r\n");
}

static errcode_t wifi_register_events_once(void)
{
    errcode_t ret;

    if (g_wifi_status.event_registered) {
        return ERRCODE_SUCC;
    }

    ret = wifi_register_event_cb(&g_wifi_event_cb);
    if (ret != ERRCODE_SUCC) {
        osal_printk(WIFI_GCODE_LOG " wifi_register_event_cb failed: 0x%x\r\n", ret);
        return ret;
    }

    g_wifi_status.event_registered = true;
    osal_printk(WIFI_GCODE_LOG " wifi event callback ready\r\n");
    return ERRCODE_SUCC;
}

static errcode_t wifi_start_softap(void)
{
    softap_config_stru hapd_conf = {0};
    softap_config_advance_stru config = {0};
    struct netif *netif_p = NULL;
    ip4_addr_t gw;
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;

    if (g_wifi_status.net_ready) {
        return ERRCODE_SUCC;
    }

    IP4_ADDR(&ipaddr, LASER_WIFI_SOFTAP_IP_ADDR_1, LASER_WIFI_SOFTAP_IP_ADDR_2, LASER_WIFI_SOFTAP_IP_ADDR_3,
             LASER_WIFI_SOFTAP_IP_ADDR_4);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, LASER_WIFI_SOFTAP_IP_ADDR_1, LASER_WIFI_SOFTAP_IP_ADDR_2, LASER_WIFI_SOFTAP_IP_ADDR_3,
             LASER_WIFI_SOFTAP_IP_ADDR_4);

    if (snprintf((char *)hapd_conf.ssid, sizeof(hapd_conf.ssid), "%s", LASER_WIFI_SOFTAP_SSID) <= 0) {
        return ERRCODE_FAIL;
    }
    if (snprintf((char *)hapd_conf.pre_shared_key, sizeof(hapd_conf.pre_shared_key), "%s", LASER_WIFI_SOFTAP_PSK) < 0) {
        return ERRCODE_FAIL;
    }
    hapd_conf.security_type = WIFI_SEC_TYPE_WPA2_WPA_PSK_MIX;
    hapd_conf.channel_num = LASER_WIFI_SOFTAP_CHANNEL;
    hapd_conf.wifi_psk_type = WIFI_WPA_PSK_NOT_USE;

    config.beacon_interval = 100;
    config.dtim_period = 2;
    config.gi = 0;
    config.group_rekey = 86400;
    config.protocol_mode = WIFI_MODE_11B_G_N_AX;
    config.hidden_ssid_flag = 1;

    if (wifi_set_softap_config_advance(&config) != ERRCODE_SUCC) {
        osal_printk(WIFI_GCODE_LOG " wifi_set_softap_config_advance failed\r\n");
        return ERRCODE_FAIL;
    }
    if (wifi_softap_enable(&hapd_conf) != ERRCODE_SUCC) {
        osal_printk(WIFI_GCODE_LOG " wifi_softap_enable failed\r\n");
        return ERRCODE_FAIL;
    }

    netif_p = netif_find(LASER_WIFI_SOFTAP_IFNAME);
    if (netif_p == NULL) {
        osal_printk(WIFI_GCODE_LOG " netif %s not found\r\n", LASER_WIFI_SOFTAP_IFNAME);
        (void)wifi_softap_disable();
        return ERRCODE_FAIL;
    }
    if (netifapi_netif_set_addr(netif_p, &ipaddr, &netmask, &gw) != 0) {
        osal_printk(WIFI_GCODE_LOG " netif set addr failed\r\n");
        (void)wifi_softap_disable();
        return ERRCODE_FAIL;
    }
    if (netifapi_dhcps_start(netif_p, NULL, 0) != 0) {
        osal_printk(WIFI_GCODE_LOG " dhcps start failed\r\n");
        (void)wifi_softap_disable();
        return ERRCODE_FAIL;
    }

    g_wifi_status.net_ready = true;
    g_wifi_status.softap_sta_count = 0;
    wifi_status_refresh_ip();
    osal_printk(WIFI_GCODE_LOG " softap ready ssid=%s ip=%s port=%u channel=%d\r\n", LASER_WIFI_SOFTAP_SSID,
                g_wifi_status.ip, LASER_WIFI_TCP_PORT, LASER_WIFI_SOFTAP_CHANNEL);
    return ERRCODE_SUCC;
}

static errcode_t wifi_start_targeted_scan(void)
{
    wifi_scan_params_stru scan_params = {0};

    scan_params.scan_type = WIFI_SSID_SCAN;
    scan_params.ssid_len = (uint8_t)strlen(LASER_WIFI_STA_SSID);
    if (snprintf((char *)scan_params.ssid, sizeof(scan_params.ssid), "%s", LASER_WIFI_STA_SSID) <= 0) {
        return ERRCODE_FAIL;
    }

    g_wifi_scan_done = false;
    if (wifi_sta_scan_advance(&scan_params) != ERRCODE_SUCC) {
        osal_printk(WIFI_GCODE_LOG " sta scan start failed for ssid=%s\r\n", LASER_WIFI_STA_SSID);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCC;
}

static bool wifi_wait_scan_done(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms <= timeout_ms) {
        if (g_wifi_scan_done) {
            return true;
        }
        osal_msleep(WIFI_GCODE_POLL_SLICE_MS);
        waited_ms += WIFI_GCODE_POLL_SLICE_MS;
    }

    osal_printk(WIFI_GCODE_LOG " sta scan timeout after %u ms\r\n", timeout_ms);
    return false;
}

static errcode_t wifi_fill_sta_target_config(wifi_sta_config_stru *expected_bss)
{
    uint32_t num = WIFI_GCODE_STA_SCAN_AP_LIMIT;
    uint32_t scan_len = sizeof(wifi_scan_info_stru) * WIFI_GCODE_STA_SCAN_AP_LIMIT;
    wifi_scan_info_stru *result = osal_kmalloc(scan_len, OSAL_GFP_ATOMIC);
    errcode_t ret = ERRCODE_FAIL;

    if (result == NULL) {
        osal_printk(WIFI_GCODE_LOG " scan result alloc fail\r\n");
        return ERRCODE_MALLOC;
    }

    memset(result, 0, scan_len);
    if (wifi_sta_get_scan_info(result, &num) != ERRCODE_SUCC) {
        osal_printk(WIFI_GCODE_LOG " wifi_sta_get_scan_info failed\r\n");
        goto out;
    }

    for (uint32_t i = 0; i < num; i++) {
        if (strcmp((const char *)result[i].ssid, LASER_WIFI_STA_SSID) != 0) {
            continue;
        }

        memset(expected_bss, 0, sizeof(*expected_bss));
        if (snprintf((char *)expected_bss->ssid, sizeof(expected_bss->ssid), "%s", LASER_WIFI_STA_SSID) <= 0) {
            goto out;
        }
        if (snprintf((char *)expected_bss->pre_shared_key, sizeof(expected_bss->pre_shared_key), "%s",
                     LASER_WIFI_STA_PSK) < 0) {
            goto out;
        }
        memcpy(expected_bss->bssid, result[i].bssid, sizeof(expected_bss->bssid));
        expected_bss->security_type = result[i].security_type;
        expected_bss->channel = (uint8_t)result[i].channel_num;
        expected_bss->ip_type = DHCP;
        ret = ERRCODE_SUCC;
        goto out;
    }

    osal_printk(WIFI_GCODE_LOG " sta target ssid not found: %s\r\n", LASER_WIFI_STA_SSID);

out:
    osal_kfree(result);
    return ret;
}

static bool wifi_wait_sta_connected(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms <= timeout_ms) {
        wifi_linked_info_stru linked_info = {0};

        if ((wifi_sta_get_ap_info(&linked_info) == ERRCODE_SUCC) && (linked_info.conn_state == WIFI_CONNECTED)) {
            g_wifi_status.sta_link_up = true;
            return true;
        }

        osal_msleep(WIFI_GCODE_POLL_SLICE_MS);
        waited_ms += WIFI_GCODE_POLL_SLICE_MS;
    }

    osal_printk(WIFI_GCODE_LOG " sta connect timeout after %u ms (reason=%d)\r\n", timeout_ms,
                g_wifi_status.last_disconnect_reason);
    return false;
}

static bool wifi_wait_sta_ip_ready(struct netif *netif_p, uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms <= timeout_ms) {
        if ((netif_p != NULL) && (ip_addr_isany(&(netif_p->ip_addr)) == 0)) {
            g_wifi_status.net_ready = true;
            wifi_status_refresh_ip();
            return true;
        }

        osal_msleep(WIFI_GCODE_POLL_SLICE_MS);
        waited_ms += WIFI_GCODE_POLL_SLICE_MS;
    }

    osal_printk(WIFI_GCODE_LOG " sta dhcp timeout after %u ms\r\n", timeout_ms);
    return false;
}

static void wifi_reset_sta_runtime(void)
{
    (void)wifi_sta_disconnect();
    (void)wifi_sta_disable();
    g_wifi_status.sta_link_up = false;
    g_wifi_status.net_ready = false;
    g_wifi_status.tcp_listening = false;
    g_wifi_status.client_connected = false;
    wifi_status_clear_ip();
}

static errcode_t wifi_start_sta(void)
{
    struct netif *netif_p = NULL;
    wifi_sta_config_stru expected_bss = {0};

    if (g_wifi_status.net_ready && wifi_is_network_ready()) {
        return ERRCODE_SUCC;
    }

    if (wifi_is_sta_enabled() == 0) {
        if (wifi_sta_enable() != ERRCODE_SUCC) {
            osal_printk(WIFI_GCODE_LOG " wifi_sta_enable failed\r\n");
            return ERRCODE_FAIL;
        }
        osal_printk(WIFI_GCODE_LOG " sta enabled, target ssid=%s\r\n", LASER_WIFI_STA_SSID);
    }

    if (wifi_start_targeted_scan() != ERRCODE_SUCC) {
        goto fail;
    }
    if (!wifi_wait_scan_done(LASER_WIFI_STA_SCAN_TIMEOUT_MS)) {
        goto fail;
    }
    if (wifi_fill_sta_target_config(&expected_bss) != ERRCODE_SUCC) {
        goto fail;
    }
    if (wifi_sta_connect(&expected_bss) != ERRCODE_SUCC) {
        osal_printk(WIFI_GCODE_LOG " wifi_sta_connect failed\r\n");
        goto fail;
    }
    if (!wifi_wait_sta_connected(LASER_WIFI_STA_CONNECT_TIMEOUT_MS)) {
        goto fail;
    }

    netif_p = netifapi_netif_find(LASER_WIFI_STA_IFNAME);
    if (netif_p == NULL) {
        osal_printk(WIFI_GCODE_LOG " netif %s not found\r\n", LASER_WIFI_STA_IFNAME);
        goto fail;
    }
    if (netifapi_dhcp_start(netif_p) != 0) {
        osal_printk(WIFI_GCODE_LOG " dhcp start failed\r\n");
        goto fail;
    }
    if (!wifi_wait_sta_ip_ready(netif_p, LASER_WIFI_STA_DHCP_TIMEOUT_MS)) {
        goto fail;
    }

    osal_printk(WIFI_GCODE_LOG " sta ready ssid=%s ip=%s port=%u\r\n", LASER_WIFI_STA_SSID, g_wifi_status.ip,
                LASER_WIFI_TCP_PORT);
    return ERRCODE_SUCC;

fail:
    wifi_reset_sta_runtime();
    return ERRCODE_FAIL;
}

static void wifi_disable_unused_mode(void)
{
    if (g_wifi_status.mode == WIFI_GCODE_MODE_STA) {
        (void)wifi_softap_disable();
    } else {
        (void)wifi_sta_disconnect();
        (void)wifi_sta_disable();
    }
}

static int wifi_open_server_socket(void)
{
    int listen_sock;
    int opt = 1;
    int sock_buf = 8192;
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = (int16_t)wifi_htons((uint16_t)LASER_WIFI_TCP_PORT);
    server_addr.sin_addr.s_addr = 0;

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        osal_printk(WIFI_GCODE_LOG " socket create failed\r\n");
        return -1;
    }

    (void)setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    (void)setsockopt(listen_sock, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf));
    (void)setsockopt(listen_sock, SOL_SOCKET, SO_SNDBUF, &sock_buf, sizeof(sock_buf));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        osal_printk(WIFI_GCODE_LOG " bind failed on port %u errno=%d\r\n", LASER_WIFI_TCP_PORT, errno);
        lwip_close(listen_sock);
        return -1;
    }

    if (listen(listen_sock, WIFI_GCODE_LISTEN_BACKLOG) < 0) {
        osal_printk(WIFI_GCODE_LOG " listen failed errno=%d\r\n", errno);
        lwip_close(listen_sock);
        return -1;
    }

    g_wifi_status.tcp_listening = true;
    osal_printk(WIFI_GCODE_LOG " tcp listen ready mode=%s ip=%s port=%u\r\n", wifi_mode_name(g_wifi_status.mode),
                g_wifi_status.ip, LASER_WIFI_TCP_PORT);
    return listen_sock;
}

static void wifi_reject_busy_client(int client_sock)
{
    wifi_send_log(client_sock, "\r\nWS63 Laser Marker WiFi\r\n");
    wifi_send_log(client_sock, "[MSG:busy another upstream host is connected]\r\n");
    wifi_send_log(client_sock, "error:busy\r\n");
}

static void wifi_reject_pending_clients(int listen_sock)
{
    while (wifi_is_network_ready()) {
        int ready = wifi_wait_socket_readable(listen_sock, 0);
        if (ready <= 0) {
            return;
        }

        {
            int pending_sock;
            struct sockaddr_in pending_addr;
            socklen_t pending_addr_len = sizeof(pending_addr);

            memset(&pending_addr, 0, sizeof(pending_addr));
            pending_sock = accept(listen_sock, (struct sockaddr *)&pending_addr, &pending_addr_len);
            if (pending_sock < 0) {
                osal_printk(WIFI_GCODE_LOG " accept extra client failed errno=%d\r\n", errno);
                return;
            }

            osal_printk(WIFI_GCODE_LOG " reject extra client: upstream already occupied\r\n");
            wifi_reject_busy_client(pending_sock);
            lwip_close(pending_sock);
        }
    }
}

static void wifi_handle_client_session(int listen_sock, int client_sock)
{
    char line_buf[WIFI_GCODE_RX_LINE_MAX];
    uint8_t recv_buf[WIFI_GCODE_RX_BUF_SIZE];
    char intro[96];
    int line_pos = 0;
    bool line_overflow = false;

    memset(line_buf, 0, sizeof(line_buf));
    (void)snprintf(intro, sizeof(intro), "[MSG:WiFi %s %s]\r\n", wifi_mode_name(g_wifi_status.mode),
                   g_wifi_status.ssid);
    wifi_send_log(client_sock, "\r\nWS63 Laser Marker WiFi\r\n");
    wifi_send_log(client_sock, "Grbl 1.1f ['$' for help]\r\n");
    wifi_send_log(client_sock, intro);
    wifi_send_log(client_sock, "[MSG:Use $WIFI? for link status]\r\n");
    wifi_send_log(client_sock, "[MSG:One upstream host at a time]\r\n");

    while (wifi_is_network_ready()) {
        wifi_reject_pending_clients(listen_sock);

        int ready = wifi_wait_socket_readable(client_sock, LASER_WIFI_SOCKET_POLL_TIMEOUT_MS);
        if (ready < 0) {
            osal_printk(WIFI_GCODE_LOG " client poll failed errno=%d\r\n", errno);
            break;
        }
        if (ready == 0) {
            continue;
        }

        {
            int recv_bytes = recv(client_sock, recv_buf, sizeof(recv_buf), 0);
            if (recv_bytes == 0) {
                osal_printk(WIFI_GCODE_LOG " client closed by peer\r\n");
                break;
            }
            if (recv_bytes < 0) {
                osal_printk(WIFI_GCODE_LOG " client recv failed errno=%d\r\n", errno);
                break;
            }
            wifi_process_rx_stream(client_sock, recv_buf, recv_bytes, line_buf, &line_pos, &line_overflow);
        }
    }
}

static void wifi_run_tcp_server_loop(void)
{
    int listen_sock = wifi_open_server_socket();
    if (listen_sock < 0) {
        g_wifi_status.tcp_listening = false;
        osal_msleep(WIFI_GCODE_RETRY_DELAY_MS);
        return;
    }

    while (wifi_is_network_ready()) {
        int ready = wifi_wait_socket_readable(listen_sock, LASER_WIFI_SOCKET_POLL_TIMEOUT_MS);
        if (ready < 0) {
            osal_printk(WIFI_GCODE_LOG " listen poll failed errno=%d\r\n", errno);
            break;
        }
        if (ready == 0) {
            continue;
        }

        {
            int client_sock;
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);

            memset(&client_addr, 0, sizeof(client_addr));
            client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
            if (client_sock < 0) {
                osal_printk(WIFI_GCODE_LOG " accept failed errno=%d\r\n", errno);
                osal_msleep(WIFI_GCODE_POLL_SLICE_MS);
                continue;
            }

            g_wifi_status.client_connected = true;
            osal_printk(WIFI_GCODE_LOG " client connected\r\n");
            wifi_handle_client_session(listen_sock, client_sock);
            g_wifi_status.client_connected = false;
            lwip_close(client_sock);
            osal_printk(WIFI_GCODE_LOG " client disconnected\r\n");
        }
    }

    g_wifi_status.tcp_listening = false;
    g_wifi_status.client_connected = false;
    lwip_close(listen_sock);
}

static void wifi_event_scan_state_changed(int32_t state, int32_t size)
{
    (void)state;
    (void)size;
    if (g_wifi_status.mode != WIFI_GCODE_MODE_STA) {
        return;
    }
    g_wifi_scan_done = true;
    osal_printk(WIFI_GCODE_LOG " sta scan done\r\n");
}

static void wifi_event_connection_changed(int32_t state, const wifi_linked_info_stru *info, int32_t reason_code)
{
    if (g_wifi_status.mode != WIFI_GCODE_MODE_STA) {
        return;
    }

    g_wifi_status.last_disconnect_reason = reason_code;
    if (state == WIFI_STATE_AVALIABLE) {
        g_wifi_status.sta_link_up = true;
        osal_printk(WIFI_GCODE_LOG " sta connected ssid=%s rssi=%d channel=%d\r\n",
                    (info != NULL) ? (const char *)info->ssid : "unknown", (info != NULL) ? info->rssi : 0,
                    (info != NULL) ? info->channel_num : 0);
    } else {
        g_wifi_status.sta_link_up = false;
        g_wifi_status.net_ready = false;
        g_wifi_status.tcp_listening = false;
        g_wifi_status.client_connected = false;
        wifi_status_clear_ip();
        osal_printk(WIFI_GCODE_LOG " sta disconnected reason=%d\r\n", reason_code);
    }
}

static void wifi_event_softap_state_changed(int32_t state)
{
    if (g_wifi_status.mode != WIFI_GCODE_MODE_SOFTAP) {
        return;
    }

    if (state == WIFI_STATE_AVALIABLE) {
        osal_printk(WIFI_GCODE_LOG " softap state available\r\n");
    } else {
        g_wifi_status.net_ready = false;
        g_wifi_status.tcp_listening = false;
        g_wifi_status.client_connected = false;
        g_wifi_status.softap_sta_count = 0;
        wifi_status_clear_ip();
        osal_printk(WIFI_GCODE_LOG " softap state unavailable\r\n");
    }
}

static void wifi_event_softap_sta_join(const wifi_sta_info_stru *info)
{
    if (g_wifi_status.mode != WIFI_GCODE_MODE_SOFTAP) {
        return;
    }

    g_wifi_status.softap_sta_count++;
    osal_printk(WIFI_GCODE_LOG " softap sta join mac=%02x:**:**:**:%02x:%02x count=%u\r\n",
                (info != NULL) ? info->mac_addr[0] : 0, (info != NULL) ? info->mac_addr[4] : 0,
                (info != NULL) ? info->mac_addr[5] : 0, g_wifi_status.softap_sta_count);
}

static void wifi_event_softap_sta_leave(const wifi_sta_info_stru *info)
{
    if (g_wifi_status.mode != WIFI_GCODE_MODE_SOFTAP) {
        return;
    }

    if (g_wifi_status.softap_sta_count > 0U) {
        g_wifi_status.softap_sta_count--;
    }
    osal_printk(WIFI_GCODE_LOG " softap sta leave mac=%02x:**:**:**:%02x:%02x count=%u\r\n",
                (info != NULL) ? info->mac_addr[0] : 0, (info != NULL) ? info->mac_addr[4] : 0,
                (info != NULL) ? info->mac_addr[5] : 0, g_wifi_status.softap_sta_count);
}

errcode_t wifi_gcode_server_init(void)
{
    wifi_status_init_defaults();
    return ERRCODE_SUCC;
}

int task_wifi_gcode_entry(void *arg)
{
    (void)arg;

    wifi_wait_until_inited();

    if (wifi_register_events_once() != ERRCODE_SUCC) {
        return -1;
    }

    wifi_status_reset_runtime();
    wifi_disable_unused_mode();

    while (1) {
        errcode_t ret = (g_wifi_status.mode == WIFI_GCODE_MODE_STA) ? wifi_start_sta() : wifi_start_softap();
        if (ret != ERRCODE_SUCC) {
            osal_printk(WIFI_GCODE_LOG " %s start failed, retry in %u ms\r\n", wifi_mode_name(g_wifi_status.mode),
                        (g_wifi_status.mode == WIFI_GCODE_MODE_STA) ? LASER_WIFI_STA_RETRY_DELAY_MS
                                                                    : WIFI_GCODE_RETRY_DELAY_MS);
            osal_msleep((g_wifi_status.mode == WIFI_GCODE_MODE_STA) ? LASER_WIFI_STA_RETRY_DELAY_MS
                                                                    : WIFI_GCODE_RETRY_DELAY_MS);
            continue;
        }

        wifi_run_tcp_server_loop();

        if (g_wifi_status.mode == WIFI_GCODE_MODE_STA) {
            wifi_reset_sta_runtime();
            osal_msleep(LASER_WIFI_STA_RETRY_DELAY_MS);
        } else {
            osal_msleep(WIFI_GCODE_RETRY_DELAY_MS);
        }
    }

    return 0;
}
