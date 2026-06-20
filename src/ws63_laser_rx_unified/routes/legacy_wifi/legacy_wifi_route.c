/**
 * @file wifi_grbl_server.c
 * @brief WiFi TCP G-code receiver for the single-board WS63 laser marker.
 */
#include "legacy_wifi_route.h"
#include "common_def.h"
#include "legacy_wifi_config.h"
#include "legacy_wifi_gcode_parser.h"
#include "legacy_wifi_gcode_processor.h"
#include "laser_ctrl.h"
#include "lwip/netifapi.h"
#include "lwip/sockets.h"
#include "legacy_wifi_motion_executor.h"
#include "preserve.h"
#include "soc_osal.h"
#include "systick.h"
#include "td_base.h"
#include "td_type.h"
#include "wifi_device.h"
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_LINE_MAX 128
#define WIFI_IFNAME_MAX_SIZE 16
#define WIFI_MAX_KEY_LEN 65
#define WIFI_MAX_SSID_LEN 33
#define GRBL_RESET_CHAR 0x18

static char g_rx_line[RX_LINE_MAX];
static int g_rx_pos = 0;
static int g_listen_sock = -1;
static volatile int g_client_sock = -1;
static unsigned long g_tcp_rx_chunks = 0;
static unsigned long g_tcp_rx_bytes = 0;
static unsigned long g_tcp_rx_realtime_q = 0;
static unsigned long g_tcp_tx_messages = 0;
static unsigned long g_tcp_tx_bytes = 0;
static unsigned long g_line_id = 0;
static unsigned long g_ok_count = 0;
static unsigned long g_error_count = 0;
static unsigned long g_status_count = 0;
static bool g_wifi_event_registered = false;
static volatile bool g_route_started = false;
#if LEGACY_WIFI_STATUS_PERIODIC
static unsigned long g_last_status_ms = 0;
#endif

static void wait_motion_idle(uint32_t timeout_ms);

static void wifi_event_scan_state_changed(int32_t state, int32_t size)
{
    unused(state);
    unused(size);
}

static void wifi_event_connection_changed(int32_t state, const wifi_linked_info_stru *info, int32_t reason_code)
{
    unused(info);
    osal_printk("[laser wifi] sta event state=%d reason=%d\r\n", state, reason_code);
}

static void wifi_event_softap_state_changed(int32_t state)
{
    osal_printk("[laser wifi] softap state=%d %s\r\n",
                state, (state == WIFI_STATE_AVALIABLE) ? "available" : "unavailable");
}

static void wifi_event_softap_sta_join(const wifi_sta_info_stru *info)
{
    osal_printk("[laser wifi] softap sta join mac=%02x:**:**:**:%02x:%02x\r\n",
                (info != NULL) ? info->mac_addr[0] : 0,
                (info != NULL) ? info->mac_addr[4] : 0,
                (info != NULL) ? info->mac_addr[5] : 0);
}

static void wifi_event_softap_sta_leave(const wifi_sta_info_stru *info)
{
    osal_printk("[laser wifi] softap sta leave mac=%02x:**:**:**:%02x:%02x\r\n",
                (info != NULL) ? info->mac_addr[0] : 0,
                (info != NULL) ? info->mac_addr[4] : 0,
                (info != NULL) ? info->mac_addr[5] : 0);
}

static wifi_event_stru g_wifi_event_cb = {
    .wifi_event_connection_changed = wifi_event_connection_changed,
    .wifi_event_scan_state_changed = wifi_event_scan_state_changed,
    .wifi_event_softap_state_changed = wifi_event_softap_state_changed,
    .wifi_event_softap_sta_join = wifi_event_softap_sta_join,
    .wifi_event_softap_sta_leave = wifi_event_softap_sta_leave,
};

static void wifi_register_events_once(void)
{
    if (g_wifi_event_registered) {
        return;
    }

    errcode_t ret = wifi_register_event_cb(&g_wifi_event_cb);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wifi] wifi_register_event_cb failed: 0x%x\r\n", ret);
        return;
    }

    g_wifi_event_registered = true;
    osal_printk("[laser wifi] wifi event callback ready\r\n");
}

static void log_tcp_rx_chunk(const uint8_t *buf, int len)
{
    g_tcp_rx_chunks++;
    g_tcp_rx_bytes += (unsigned long)len;

    if (len == 1 && buf[0] == '?') {
        g_tcp_rx_realtime_q++;
        if ((g_tcp_rx_realtime_q & 0x0fUL) == 1) {
            osal_printk("[WIFI_RT] '?' count=%lu chunks=%lu bytes=%lu\r\n",
                        g_tcp_rx_realtime_q, g_tcp_rx_chunks, g_tcp_rx_bytes);
        }
        return;
    }

    char preview[96];
    int out = 0;
    int limit = (len < 40) ? len : 40;
    for (int i = 0; i < limit && out < (int)sizeof(preview) - 5; i++) {
        uint8_t ch = buf[i];
        if (ch == '\r') {
            preview[out++] = '\\';
            preview[out++] = 'r';
        } else if (ch == '\n') {
            preview[out++] = '\\';
            preview[out++] = 'n';
        } else if (ch == GRBL_RESET_CHAR) {
            preview[out++] = '^';
            preview[out++] = 'X';
        } else if (isprint(ch)) {
            preview[out++] = (char)ch;
        } else {
            out += snprintf(&preview[out], sizeof(preview) - (size_t)out, "\\x%02X", ch);
        }
    }
    preview[out] = '\0';
    osal_printk("[WIFI_TCP_RX] chunk=%lu len=%d total=%lu data=\"%s%s\"\r\n",
                g_tcp_rx_chunks, len, g_tcp_rx_bytes, preview, (len > limit) ? "..." : "");
}

static void wifi_send_str(const char *str)
{
    int sock = g_client_sock;
    const char *ptr = str;
    size_t left = strlen(str);
    size_t total = left;

    while (sock >= 0 && left > 0) {
        int sent = send(sock, ptr, left, 0);
        if (sent <= 0) {
            osal_printk("[laser wifi] tcp send failed errno=%d\r\n", errno);
            return;
        }
        ptr += sent;
        left -= (size_t)sent;
    }

    if (total > 0) {
        g_tcp_tx_messages++;
        g_tcp_tx_bytes += (unsigned long)total;
    }
}

static void send_ok(void)
{
    g_ok_count++;
    osal_printk("[WIFI_OK] count=%lu tx_msg=%lu tx_bytes=%lu\r\n",
                g_ok_count, g_tcp_tx_messages, g_tcp_tx_bytes);
    wifi_send_str("ok\r\n");
}

static void send_error(int code)
{
    char buf[24];
    g_error_count++;
    osal_printk("[WIFI_ERROR] count=%lu code=%d\r\n", g_error_count, code);
    snprintf(buf, sizeof(buf), "error:%d\r\n", code);
    wifi_send_str(buf);
}

static bool enqueue_motion_cmd(const legacy_wifi_motion_cmd_t *cmd)
{
    if (!legacy_wifi_motion_executor_enqueue(cmd)) {
        send_error(9);
        return false;
    }
    return true;
}

static void send_grbl_startup(const char *source)
{
    char buf[112];

    wifi_send_str("\r\nGrbl 1.1f ['$' for help]\r\n");
    snprintf(buf, sizeof(buf), "[MSG:WS63 WiFi Laser ready source=%s uptime=%lums reset=0x%04x count=%u]\r\n",
             source, (unsigned long)uapi_systick_get_ms(), (unsigned int)get_cpu_utils_reset_cause(),
             get_cpu_utils_reset_count());
    wifi_send_str(buf);
}

static bool parsed_line_contains_gcode(const legacy_wifi_gcode_line_t *gc, int expected_code)
{
    const char *p = gc->line;
    while ((p = strchr(p, 'G')) != NULL) {
        if ((p == gc->line || !(((*(p - 1) >= 'A') && (*(p - 1) <= 'Z')) ||
                                ((*(p - 1) >= 'a') && (*(p - 1) <= 'z')))) &&
            atoi(p + 1) == expected_code) {
            return true;
        }
        p++;
    }
    return false;
}

static bool line_contains_mcode(const char *line, int expected_code)
{
    const char *p = line;
    while ((p = strchr(p, 'M')) != NULL) {
        if ((p == line || !isalpha((unsigned char)*(p - 1))) && atoi(p + 1) == expected_code) {
            return true;
        }
        p++;
    }
    return false;
}

static bool machine_is_idle(void)
{
    if (legacy_wifi_motion_executor_is_busy()) {
        return false;
    }
    unsigned long last = legacy_wifi_motion_executor_last_activity_ms();
    if (last == 0) {
        return true;
    }
    return ((unsigned long)uapi_systick_get_ms() - last) > LEGACY_WIFI_ACTIVITY_TIMEOUT_MS;
}

static void send_settings_report(void)
{
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "$0=10\r\n"
        "$1=25\r\n"
        "$2=0\r\n"
        "$3=0\r\n"
        "$4=0\r\n"
        "$5=0\r\n"
        "$6=0\r\n"
        "$10=1\r\n"
        "$11=0.010\r\n"
        "$12=0.002\r\n"
        "$13=0\r\n"
        "$20=0\r\n"
        "$21=0\r\n"
        "$22=0\r\n"
        "$23=0\r\n"
        "$24=25.000\r\n"
        "$25=500.000\r\n"
        "$26=250\r\n"
        "$27=1.000\r\n"
        "$30=1000\r\n"
        "$31=0\r\n"
        "$32=1\r\n"
        "$100=80.000\r\n"
        "$101=80.000\r\n"
        "$102=250.000\r\n"
        "$110=8000.000\r\n"
        "$111=8000.000\r\n"
        "$112=500.000\r\n"
        "$120=1000.000\r\n"
        "$121=1000.000\r\n"
        "$122=10.000\r\n"
        "$130=%.3f\r\n"
        "$131=%.3f\r\n"
        "$132=0.000\r\n"
        "ok\r\n",
        LEGACY_WIFI_GALVO_WORK_AREA_X_MM, LEGACY_WIFI_GALVO_WORK_AREA_Y_MM);
    if (n > 0) {
        wifi_send_str(buf);
    }
}

static void send_coordinate_report(void)
{
    wifi_send_str("[G54:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G55:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G56:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G57:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G58:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G59:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G28:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G30:0.000,0.000,0.000]\r\n");
    wifi_send_str("[G92:0.000,0.000,0.000]\r\n");
    wifi_send_str("[TLO:0.000]\r\n");
    wifi_send_str("[PRB:0.000,0.000,0.000:0]\r\n");
    send_ok();
}

static void send_startup_blocks(void)
{
    wifi_send_str("$N0=\r\n");
    wifi_send_str("$N1=\r\n");
    send_ok();
}

static void send_status_report(void)
{
    char buf[128];
    const char *state = machine_is_idle() ? "Idle" : "Run";
    g_status_count++;
    snprintf(buf, sizeof(buf), "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d|Ln:%lu>\r\n", state,
             legacy_wifi_motion_executor_get_x(), legacy_wifi_motion_executor_get_y(), (int)legacy_wifi_gcode_processor_get_feed_rate(),
             (int)legacy_wifi_gcode_processor_get_laser_power(), legacy_wifi_gcode_processor_get_line_count());
    wifi_send_str(buf);
    if ((g_status_count & 0x0fUL) == 1) {
        osal_printk("[WIFI_STATUS] count=%lu state=%s x=%.3f y=%.3f q=%u busy=%d\r\n",
                    g_status_count, state, legacy_wifi_motion_executor_get_x(), legacy_wifi_motion_executor_get_y(),
                    (unsigned int)legacy_wifi_motion_executor_queue_depth(), legacy_wifi_motion_executor_is_busy() ? 1 : 0);
    }
}

static void send_periodic_status(void)
{
#if LEGACY_WIFI_STATUS_PERIODIC
    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - g_last_status_ms) >= LEGACY_WIFI_STATUS_INTERVAL_MS) {
        send_status_report();
        g_last_status_ms = now;
    }
#endif
}

static void send_wait_status(unsigned long *last_status_ms)
{
    if (last_status_ms == NULL) {
        return;
    }

    unsigned long now = (unsigned long)uapi_systick_get_ms();
    if ((now - *last_status_ms) >= LEGACY_WIFI_STATUS_INTERVAL_MS) {
        send_status_report();
        *last_status_ms = now;
    }
}

static bool handle_dollar_command(const char *line)
{
    char buf[768];

    if (line[0] != '$') {
        return false;
    }

    if (strncmp(line, "$J=", 3) == 0) {
        legacy_wifi_gcode_line_t gc;
        legacy_wifi_gcode_init(&gc);
        for (int i = 3; line[i] != '\0' && gc.len < LEGACY_WIFI_GCODE_LINE_MAX - 1; i++) {
            legacy_wifi_gcode_add_char(&gc, line[i]);
        }
        if (!legacy_wifi_gcode_parse(&gc)) {
            send_error(2);
            return true;
        }

        bool relative = parsed_line_contains_gcode(&gc, 91);
        double target_x = legacy_wifi_motion_executor_get_x();
        double target_y = legacy_wifi_motion_executor_get_y();
        double feed_rate = legacy_wifi_gcode_has_word(&gc, 'F') ? legacy_wifi_gcode_get_value(&gc, 'F') : legacy_wifi_gcode_processor_get_feed_rate();
        bool has_move = false;

        if (legacy_wifi_gcode_has_word(&gc, 'X')) {
            double x = legacy_wifi_gcode_get_value(&gc, 'X');
            target_x = relative ? (target_x + x) : x;
            has_move = true;
        }
        if (legacy_wifi_gcode_has_word(&gc, 'Y')) {
            double y = legacy_wifi_gcode_get_value(&gc, 'Y');
            target_y = relative ? (target_y + y) : y;
            has_move = true;
        }
        if (!has_move || feed_rate <= 0.0) {
            send_error(2);
            return true;
        }

        legacy_wifi_motion_cmd_t cmd = {0};
        cmd.cmd = LEGACY_WIFI_CMD_G1_MOVE;
        cmd.target_x = (float)target_x;
        cmd.target_y = (float)target_y;
        cmd.feed_rate = (float)feed_rate;
        if (enqueue_motion_cmd(&cmd)) {
            send_ok();
        }
    } else if (strcmp(line, "$FRAME") == 0) {
        legacy_wifi_motion_cmd_t cmd = {0};

        cmd.cmd = LEGACY_WIFI_CMD_G0_MOVE;
        cmd.target_x = (float)LEGACY_WIFI_GALVO_X_MIN_MM;
        cmd.target_y = (float)LEGACY_WIFI_GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }

        cmd.cmd = LEGACY_WIFI_CMD_G1_MOVE;
        cmd.feed_rate = (float)LEGACY_WIFI_FRAME_FEED_RATE;
        cmd.flags = LEGACY_WIFI_FLAG_LASER_ON;
        cmd.laser_pwr = LEGACY_WIFI_FRAME_LASER_POWER;
        cmd.target_x = (float)LEGACY_WIFI_GALVO_X_MAX_MM;
        cmd.target_y = (float)LEGACY_WIFI_GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)LEGACY_WIFI_GALVO_X_MAX_MM;
        cmd.target_y = (float)LEGACY_WIFI_GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)LEGACY_WIFI_GALVO_X_MIN_MM;
        cmd.target_y = (float)LEGACY_WIFI_GALVO_Y_MAX_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        cmd.target_x = (float)LEGACY_WIFI_GALVO_X_MIN_MM;
        cmd.target_y = (float)LEGACY_WIFI_GALVO_Y_MIN_MM;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }

        cmd.cmd = LEGACY_WIFI_CMD_LASER_OFF;
        cmd.flags = 0;
        cmd.laser_pwr = 0;
        if (!enqueue_motion_cmd(&cmd)) {
            return true;
        }
        send_ok();
    } else if (strcmp(line, "$$") == 0) {
        send_settings_report();
    } else if (strcmp(line, "$#") == 0) {
        send_coordinate_report();
    } else if (strcmp(line, "$N") == 0) {
        send_startup_blocks();
    } else if (strcmp(line, "$I") == 0) {
        wifi_send_str("[VER:1.1f.WS63_WIFI:]\r\n[OPT:V,15,128]\r\nok\r\n");
    } else if (strcmp(line, "$G") == 0) {
        snprintf(buf, sizeof(buf), "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n",
                 legacy_wifi_gcode_processor_is_absolute_mode() ? 90 : 91, legacy_wifi_gcode_processor_laser_is_enabled() ? 3 : 5,
                 (int)legacy_wifi_gcode_processor_get_feed_rate(), (int)legacy_wifi_gcode_processor_get_laser_power());
        wifi_send_str(buf);
    } else if (strcmp(line, "$D") == 0) {
        snprintf(buf, sizeof(buf),
                 "[MSG:wifi motion busy=%d queue=%u abort=%d worker=%d enq=%lu exe=%lu x=%.3f y=%.3f laser=%d power=%u pwm=%d pclk=%lu period=%lu high=%lu low=%lu req=%u eff=%u late_max=%lu late_cnt=%lu slip=%lu seg=%lu short=%lu tcp_rx_chunks=%lu tcp_rx_bytes=%lu qmark=%lu tcp_tx_msg=%lu tcp_tx_bytes=%lu lines=%lu ok=%lu err=%lu status=%lu]\r\nok\r\n",
                 legacy_wifi_motion_executor_is_busy() ? 1 : 0, (unsigned int)legacy_wifi_motion_executor_queue_depth(),
                 legacy_wifi_motion_executor_abort_requested() ? 1 : 0, legacy_wifi_motion_executor_worker_started() ? 1 : 0,
                 legacy_wifi_motion_executor_enqueued_count(), legacy_wifi_motion_executor_executed_count(),
                 legacy_wifi_motion_executor_get_x(), legacy_wifi_motion_executor_get_y(), laser_is_enabled() ? 1 : 0,
                 (unsigned int)laser_get_power(), laser_pwm_is_opened() ? 1 : 0,
                 (unsigned long)laser_pwm_clock_hz(), (unsigned long)laser_pwm_period_ticks(),
                 (unsigned long)laser_pwm_high_ticks(), (unsigned long)laser_pwm_low_ticks(),
                 (unsigned int)laser_pwm_last_requested_power(), (unsigned int)laser_pwm_last_effective_power(),
                 legacy_wifi_motion_executor_max_sample_late_us(), legacy_wifi_motion_executor_late_sample_count(),
                 legacy_wifi_motion_executor_missed_sample_count(), legacy_wifi_motion_executor_motion_segment_count(),
                 legacy_wifi_motion_executor_short_segment_count(), g_tcp_rx_chunks, g_tcp_rx_bytes,
                 g_tcp_rx_realtime_q, g_tcp_tx_messages, g_tcp_tx_bytes, g_line_id,
                 g_ok_count, g_error_count, g_status_count);
        wifi_send_str(buf);
    } else if (strcmp(line, "$H") == 0) {
        wait_motion_idle(LEGACY_WIFI_MOTION_END_DRAIN_TIMEOUT_MS);
        legacy_wifi_gcode_processor_set_origin();
        legacy_wifi_motion_executor_set_origin();
        send_ok();
    } else if (strcmp(line, "$C") == 0) {
        wifi_send_str("[GC:G0 G54 G17 G21 G90 G94 M5]\r\nok\r\n");
    } else if (strcmp(line, "$X") == 0) {
        wifi_send_str("[MSG:Caution: Unlocked]\r\nok\r\n");
    } else if (strcmp(line, "$RST=$") == 0 || strcmp(line, "$RST#") == 0 || strcmp(line, "$RST*") == 0) {
        send_ok();
    } else if (strcmp(line, "$") == 0) {
        wifi_send_str("$G - View gcode parser state\r\n");
        wifi_send_str("$I - View build info\r\n");
        wifi_send_str("$$ - View Grbl settings\r\n");
        wifi_send_str("$# - View coordinate parameters\r\n");
        wifi_send_str("$N - View startup blocks\r\n");
        wifi_send_str("$D - View motion debug state\r\n");
        wifi_send_str("$X - Kill alarm lock\r\n");
        wifi_send_str("$H - Set origin\r\n");
        send_ok();
    } else {
        send_ok();
    }

    return true;
}

static void handle_emergency_stop(void)
{
    legacy_wifi_motion_cmd_t cmd;
    osal_printk("[WIFI_SAFE_STOP] emergency stop\r\n");
    legacy_wifi_gcode_processor_build_emergency_stop(&cmd);
    legacy_wifi_motion_executor_flush();
    legacy_wifi_motion_executor_execute(&cmd);
    laser_force_off();
}

static void wait_motion_idle(uint32_t timeout_ms)
{
    unsigned long start = (unsigned long)uapi_systick_get_ms();
    unsigned long last_status_ms = 0;

    while (legacy_wifi_motion_executor_is_busy()) {
        if (((unsigned long)uapi_systick_get_ms() - start) >= timeout_ms) {
            break;
        }
        send_wait_status(&last_status_ms);
        osal_msleep(1);
    }
}

static bool handle_realtime_char(uint8_t ch)
{
    switch (ch) {
        case '?':
            send_status_report();
            return true;
        case '!':
            handle_emergency_stop();
            return true;
        case '~':
            return true;
        case GRBL_RESET_CHAR:
            g_rx_pos = 0;
            handle_emergency_stop();
            wait_motion_idle(100);
            legacy_wifi_gcode_processor_init();
            legacy_wifi_motion_executor_set_origin();
            send_grbl_startup("soft-reset");
            return true;
        default:
            return false;
    }
}

static void wait_motion_queue_watermark(void)
{
    while (legacy_wifi_motion_executor_queue_depth() >= LEGACY_WIFI_MOTION_QUEUE_OK_WATERMARK) {
        send_periodic_status();
        osal_msleep(1);
    }
}

static void execute_gcode_line(const char *line, int len)
{
    legacy_wifi_motion_cmd_t cmds[4];
    int cmd_count = 0;
    bool drain_before_ok = line_contains_mcode(line, 5);

    if (legacy_wifi_gcode_process_line(line, len, cmds, 4, &cmd_count)) {
        osal_printk("[WIFI_PARSE] id=%lu cmds=%d drain=%d line=\"%s\"\r\n",
                    g_line_id, cmd_count, drain_before_ok ? 1 : 0, line);
        for (int i = 0; i < cmd_count; i++) {
            if (!enqueue_motion_cmd(&cmds[i])) {
                return;
            }
        }
        if (cmd_count > 0) {
            wait_motion_queue_watermark();
        }
        if (drain_before_ok) {
            wait_motion_idle(LEGACY_WIFI_MOTION_END_DRAIN_TIMEOUT_MS);
            laser_force_off();
        }
    }

    send_ok();
}

static void process_line(const char *line, int len)
{
    if (len == 0) {
        return;
    }

    g_line_id++;
    osal_printk("[WIFI_LINE] id=%lu len=%d line=\"%s\"\r\n", g_line_id, len, line);

    if (strcmp(line, "?") == 0) {
        send_status_report();
        return;
    }

    if (strcmp(line, "!") == 0 || strcmp(line, "$STOP") == 0 || strcmp(line, "M112") == 0) {
        handle_emergency_stop();
        send_ok();
        return;
    }

    if (handle_dollar_command(line)) {
        return;
    }

    if (len >= RX_LINE_MAX - 1) {
        send_error(1);
        return;
    }

    execute_gcode_line(line, len);
}

static void process_byte(uint8_t ch)
{
    if (handle_realtime_char(ch)) {
        return;
    }

    if (ch == '\n' || ch == '\r') {
        if (g_rx_pos > 0) {
            g_rx_line[g_rx_pos] = '\0';
            process_line(g_rx_line, g_rx_pos);
            g_rx_pos = 0;
        }
    } else if (g_rx_pos < RX_LINE_MAX - 1) {
        g_rx_line[g_rx_pos++] = (char)ch;
    } else {
        g_rx_pos = 0;
        send_error(1);
    }
}

static int start_softap(void)
{
    softap_config_stru hapd_conf = {0};
    softap_config_advance_stru config = {0};
    td_char ifname[WIFI_IFNAME_MAX_SIZE + 1] = LEGACY_WIFI_AP_IFNAME;
    struct netif *netif_p = TD_NULL;
    ip4_addr_t gw;
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;

    while (wifi_is_wifi_inited() == 0) {
        osal_msleep(10);
    }
    osal_printk("[laser wifi] wifi init ready\r\n");
    wifi_register_events_once();

    (void)memcpy_s(hapd_conf.ssid, sizeof(hapd_conf.ssid), LEGACY_WIFI_AP_SSID,
                   strlen(LEGACY_WIFI_AP_SSID) + 1);
    (void)memcpy_s(hapd_conf.pre_shared_key, WIFI_MAX_KEY_LEN, LEGACY_WIFI_AP_PSK,
                   strlen(LEGACY_WIFI_AP_PSK) + 1);
    hapd_conf.security_type = 3;
    hapd_conf.channel_num = LEGACY_WIFI_AP_CHANNEL;
    hapd_conf.wifi_psk_type = 0;

    config.beacon_interval = 100;
    config.dtim_period = 2;
    config.gi = 0;
    config.group_rekey = 86400;
    config.protocol_mode = 4;
    config.hidden_ssid_flag = LEGACY_WIFI_AP_HIDDEN_FLAG;
    if (wifi_set_softap_config_advance(&config) != 0) {
        osal_printk("[laser wifi] set softap advance failed\r\n");
        return -1;
    }
    if (wifi_softap_enable(&hapd_conf) != 0) {
        osal_printk("[laser wifi] enable softap failed\r\n");
        return -1;
    }

    IP4_ADDR(&ipaddr, LEGACY_WIFI_AP_IP_A, LEGACY_WIFI_AP_IP_B, LEGACY_WIFI_AP_IP_C, LEGACY_WIFI_AP_IP_D);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, LEGACY_WIFI_AP_IP_A, LEGACY_WIFI_AP_IP_B, LEGACY_WIFI_AP_IP_C, LEGACY_WIFI_AP_GW_D);

    netif_p = netif_find(ifname);
    if (netif_p == TD_NULL) {
        osal_printk("[laser wifi] netif %s not found\r\n", ifname);
        (void)wifi_softap_disable();
        return -1;
    }
    if (netifapi_netif_set_addr(netif_p, &ipaddr, &netmask, &gw) != 0) {
        osal_printk("[laser wifi] set netif addr failed\r\n");
        (void)wifi_softap_disable();
        return -1;
    }
    if (netifapi_dhcps_start(netif_p, NULL, 0) != 0) {
        osal_printk("[laser wifi] start dhcp server failed\r\n");
        (void)wifi_softap_disable();
        return -1;
    }

    osal_printk("[laser wifi] softap ssid=%s ip=%d.%d.%d.%d port=%d channel=%d hidden_flag=%d\r\n", LEGACY_WIFI_AP_SSID,
                LEGACY_WIFI_AP_IP_A, LEGACY_WIFI_AP_IP_B, LEGACY_WIFI_AP_IP_C, LEGACY_WIFI_AP_IP_D,
                LEGACY_WIFI_TCP_PORT, LEGACY_WIFI_AP_CHANNEL, config.hidden_ssid_flag);
    return 0;
}

static int start_tcp_server(void)
{
    struct sockaddr_in server_addr = {0};
    int opt = 1;
    int sock_buf = LEGACY_WIFI_TCP_SOCK_BUF_SIZE;

    g_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_sock < 0) {
        osal_printk("[laser wifi] socket failed errno=%d\r\n", errno);
        return -1;
    }

    (void)setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    (void)setsockopt(g_listen_sock, SOL_SOCKET, SO_SNDBUF, &sock_buf, sizeof(sock_buf));
    (void)setsockopt(g_listen_sock, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(LEGACY_WIFI_TCP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        osal_printk("[laser wifi] bind failed errno=%d\r\n", errno);
        lwip_close(g_listen_sock);
        g_listen_sock = -1;
        return -1;
    }
    if (listen(g_listen_sock, LEGACY_WIFI_TCP_BACKLOG) < 0) {
        osal_printk("[laser wifi] listen failed errno=%d\r\n", errno);
        lwip_close(g_listen_sock);
        g_listen_sock = -1;
        return -1;
    }

    osal_printk("[laser wifi] tcp server listening port=%d\r\n", LEGACY_WIFI_TCP_PORT);
    return 0;
}

static void close_client(void)
{
    int sock = g_client_sock;
    g_client_sock = -1;
    if (sock >= 0) {
        lwip_close(sock);
    }
    osal_printk("[WIFI_CLIENT_CLOSE] rx_chunks=%lu rx_bytes=%lu tx_msg=%lu tx_bytes=%lu lines=%lu ok=%lu err=%lu status=%lu\r\n",
                g_tcp_rx_chunks, g_tcp_rx_bytes, g_tcp_tx_messages, g_tcp_tx_bytes,
                g_line_id, g_ok_count, g_error_count, g_status_count);
    g_rx_pos = 0;
    legacy_wifi_motion_executor_flush();
    laser_force_off();
}

int legacy_wifi_route_task_entry(void *arg)
{
    unused(arg);

    osal_printk("[laser wifi] task started\r\n");
    if (start_softap() != 0 || start_tcp_server() != 0) {
        return -1;
    }

    while (1) {
        struct sockaddr_in client_addr = {0};
        socklen_t addr_len = sizeof(client_addr);
        uint8_t buf[LEGACY_WIFI_TCP_RX_BUF_SIZE];

        osal_printk("[laser wifi] waiting tcp client\r\n");
        int client = accept(g_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            osal_printk("[laser wifi] accept failed errno=%d\r\n", errno);
            osal_msleep(100);
            continue;
        }

        g_client_sock = client;
        g_rx_pos = 0;
        g_tcp_rx_chunks = 0;
        g_tcp_rx_bytes = 0;
        g_tcp_rx_realtime_q = 0;
        g_tcp_tx_messages = 0;
        g_tcp_tx_bytes = 0;
        g_line_id = 0;
        g_ok_count = 0;
        g_error_count = 0;
        g_status_count = 0;
        osal_printk("[laser wifi] client connected sock=%d\r\n", client);
        send_grbl_startup("tcp-connect");

        while (g_client_sock >= 0) {
            int ret = recv(client, buf, sizeof(buf), 0);
            if (ret <= 0) {
                osal_printk("[laser wifi] client disconnected ret=%d errno=%d\r\n", ret, errno);
                break;
            }
            log_tcp_rx_chunk(buf, ret);
            for (int i = 0; i < ret; i++) {
                process_byte(buf[i]);
            }
            send_periodic_status();
        }

        close_client();
    }

    return 0;
}

errcode_t legacy_wifi_route_init(void)
{
    osal_printk("[LEGACY_WIFI] legacy_wifi_route_init OK ssid=%s ip=%d.%d.%d.%d port=%d channel=%d hidden_flag=%d\r\n",
                LEGACY_WIFI_AP_SSID, LEGACY_WIFI_AP_IP_A, LEGACY_WIFI_AP_IP_B, LEGACY_WIFI_AP_IP_C,
                LEGACY_WIFI_AP_IP_D, LEGACY_WIFI_TCP_PORT, LEGACY_WIFI_AP_CHANNEL, LEGACY_WIFI_AP_HIDDEN_FLAG);
    return ERRCODE_SUCC;
}

errcode_t legacy_wifi_route_start(void)
{
    if (g_route_started) {
        osal_printk("[LEGACY_WIFI] route already started\r\n");
        return ERRCODE_SUCC;
    }

    laser_force_off();
    legacy_wifi_gcode_processor_init();
    legacy_wifi_motion_executor_init();

    errcode_t ret = legacy_wifi_route_init();
    if (ret != ERRCODE_SUCC) {
        laser_force_off();
        return ret;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(legacy_wifi_route_task_entry, NULL, "legacy_wifi",
                                          LEGACY_WIFI_TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[LEGACY_WIFI] create WiFi TCP task failed\r\n");
        laser_force_off();
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, LEGACY_WIFI_TASK_PRIO_WIFI) != OSAL_SUCCESS) {
        osal_printk("[LEGACY_WIFI] set WiFi TCP priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    ret = legacy_wifi_motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LEGACY_WIFI] legacy_wifi_motion_executor_start_task failed: 0x%x\r\n", ret);
        legacy_wifi_route_force_stop();
        return ret;
    }

    g_route_started = true;
    osal_printk("[LEGACY_WIFI] route started\r\n");
    return ERRCODE_SUCC;
}

bool legacy_wifi_route_is_idle(void)
{
    if (!g_route_started) {
        return true;
    }
    return !legacy_wifi_motion_executor_is_busy() && !legacy_wifi_motion_executor_abort_requested();
}

void legacy_wifi_route_force_stop(void)
{
    if (g_client_sock >= 0) {
        close_client();
    }
    if (g_listen_sock >= 0) {
        lwip_close(g_listen_sock);
        g_listen_sock = -1;
    }
    legacy_wifi_motion_executor_request_abort();
    legacy_wifi_motion_executor_flush();
    laser_force_off();
}
