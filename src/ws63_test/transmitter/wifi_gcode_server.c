/**
 * @file wifi_gcode_server.c
 * @brief 发射板 WiFi G-Code 入口
 *        参考官方 wifi/softap_sample，在发射板上增加 SoftAP + TCP 文本接入。
 *        这里故意不修改原有 UART/SLE 代码路径，而是增加一个并行入口，
 *        方便在不破坏既有串口联调基线的前提下引入 WiFi 控制能力。
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

#include <stdio.h>
#include <string.h>

#define WIFI_GCODE_LOG "[wifi gcode]"
#define WIFI_GCODE_IFNAME "ap0"
#define WIFI_GCODE_RX_LINE_MAX 128
#define WIFI_GCODE_RX_BUF_SIZE 256
#define WIFI_GCODE_LISTEN_BACKLOG 1
#define WIFI_GCODE_RETRY_DELAY_MS 1000
#define WIFI_GCODE_ACCEPT_RETRY_DELAY_MS 100

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
    osal_printk(
        WIFI_GCODE_LOG
        " %s: cmd=0x%x conn=%u handles=%u status_rx=%u cmd_hdl=0x%x status_hdl=0x%x pending=%u queue_free=%u ack=%u\r\n",
        reason, (cmd != NULL) ? cmd->cmd : 0, sle_laser_client_is_connected() ? 1U : 0U,
        sle_laser_client_has_handles_ready() ? 1U : 0U, sle_laser_client_has_status_rx() ? 1U : 0U,
        sle_laser_client_get_cmd_handle(), sle_laser_client_get_status_handle(),
        sle_laser_client_get_pending_writes(), sle_laser_client_get_queue_free(), sle_laser_client_get_last_ack_seq());
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
                    while (sle_laser_client_is_ready() && sle_laser_client_get_queue_free() < FLOW_CTRL_PAUSE_THRESHOLD) {
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

static void wifi_process_rx_stream(int client_sock, const uint8_t *buf, int len, char *line_buf, int *line_pos)
{
    for (int i = 0; i < len; i++) {
        uint8_t ch = buf[i];

        if (ch == '\n' || ch == '\r') {
            if (*line_pos > 0) {
                line_buf[*line_pos] = '\0';
                wifi_process_line(client_sock, line_buf, *line_pos);
                *line_pos = 0;
            }
            continue;
        }

        if (*line_pos < (WIFI_GCODE_RX_LINE_MAX - 1)) {
            line_buf[*line_pos] = (char)ch;
            (*line_pos)++;
        }
    }
}

static uint16_t wifi_htons(uint16_t value)
{
    return (uint16_t)(((value & 0x00FFU) << 8) | ((value & 0xFF00U) >> 8));
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

static errcode_t wifi_start_softap(void)
{
    softap_config_stru hapd_conf = {0};
    softap_config_advance_stru config = {0};
    struct netif *netif_p = NULL;
    ip4_addr_t gw;
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;

    IP4_ADDR(&ipaddr, LASER_WIFI_SOFTAP_IP_ADDR_1, LASER_WIFI_SOFTAP_IP_ADDR_2, LASER_WIFI_SOFTAP_IP_ADDR_3,
             LASER_WIFI_SOFTAP_IP_ADDR_4);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, LASER_WIFI_SOFTAP_IP_ADDR_1, LASER_WIFI_SOFTAP_IP_ADDR_2, LASER_WIFI_SOFTAP_IP_ADDR_3,
             LASER_WIFI_SOFTAP_IP_ADDR_4);

    if (snprintf((char *)hapd_conf.ssid, sizeof(hapd_conf.ssid), "%s", LASER_WIFI_SOFTAP_SSID) <= 0) {
        return ERRCODE_FAIL;
    }
    if (snprintf((char *)hapd_conf.pre_shared_key, sizeof(hapd_conf.pre_shared_key), "%s", LASER_WIFI_SOFTAP_PSK) <
        0) {
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

    netif_p = netif_find(WIFI_GCODE_IFNAME);
    if (netif_p == NULL) {
        osal_printk(WIFI_GCODE_LOG " netif ap0 not found\r\n");
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

    osal_printk(WIFI_GCODE_LOG " softap ready ssid=%s ip=%u.%u.%u.%u port=%u channel=%d\r\n",
                LASER_WIFI_SOFTAP_SSID, LASER_WIFI_SOFTAP_IP_ADDR_1, LASER_WIFI_SOFTAP_IP_ADDR_2,
                LASER_WIFI_SOFTAP_IP_ADDR_3, LASER_WIFI_SOFTAP_IP_ADDR_4, LASER_WIFI_TCP_PORT,
                LASER_WIFI_SOFTAP_CHANNEL);
    return ERRCODE_SUCC;
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
        osal_printk(WIFI_GCODE_LOG " bind failed on port %u\r\n", LASER_WIFI_TCP_PORT);
        lwip_close(listen_sock);
        return -1;
    }

    if (listen(listen_sock, WIFI_GCODE_LISTEN_BACKLOG) < 0) {
        osal_printk(WIFI_GCODE_LOG " listen failed\r\n");
        lwip_close(listen_sock);
        return -1;
    }

    osal_printk(WIFI_GCODE_LOG " tcp listen ready on port %u\r\n", LASER_WIFI_TCP_PORT);
    return listen_sock;
}

static void wifi_handle_client_session(int client_sock)
{
    char line_buf[WIFI_GCODE_RX_LINE_MAX];
    uint8_t recv_buf[WIFI_GCODE_RX_BUF_SIZE];
    int line_pos = 0;

    memset(line_buf, 0, sizeof(line_buf));
    wifi_send_log(client_sock, "\r\nWS63 Laser Marker WiFi\r\n");
    wifi_send_log(client_sock, "Grbl 1.1f ['$' for help]\r\n");
    wifi_send_log(client_sock, "[MSG:One upstream host at a time]\r\n");

    while (1) {
        int recv_bytes = recv(client_sock, recv_buf, sizeof(recv_buf), 0);
        if (recv_bytes <= 0) {
            break;
        }
        wifi_process_rx_stream(client_sock, recv_buf, recv_bytes, line_buf, &line_pos);
    }
}

static void wifi_run_tcp_server_loop(void)
{
    int listen_sock = wifi_open_server_socket();
    if (listen_sock < 0) {
        osal_msleep(WIFI_GCODE_RETRY_DELAY_MS);
        return;
    }

    while (1) {
        int client_sock;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        memset(&client_addr, 0, sizeof(client_addr));
        client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            osal_printk(WIFI_GCODE_LOG " accept failed\r\n");
            osal_msleep(WIFI_GCODE_ACCEPT_RETRY_DELAY_MS);
            continue;
        }

        osal_printk(WIFI_GCODE_LOG " client connected\r\n");
        wifi_handle_client_session(client_sock);
        lwip_close(client_sock);
        osal_printk(WIFI_GCODE_LOG " client disconnected\r\n");
    }
}

errcode_t wifi_gcode_server_init(void)
{
    return ERRCODE_SUCC;
}

int task_wifi_gcode_entry(void *arg)
{
    (void)arg;

    wifi_wait_until_inited();

    while (wifi_start_softap() != ERRCODE_SUCC) {
        osal_printk(WIFI_GCODE_LOG " softap start failed, retry in %u ms\r\n", WIFI_GCODE_RETRY_DELAY_MS);
        osal_msleep(WIFI_GCODE_RETRY_DELAY_MS);
    }

    while (1) {
        wifi_run_tcp_server_loop();
    }

    return 0;
}
