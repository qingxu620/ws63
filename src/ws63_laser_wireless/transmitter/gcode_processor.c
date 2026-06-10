/**
 * @file gcode_processor.c
 * @brief Convert LaserGRBL-compatible G-code into wireless motion packets.
 */
#include "gcode_processor.h"
#include "config.h"
#include "gcode_parser.h"
#include "soc_osal.h"
#include "systick.h"
#include "wireless_crc16.h"
#include <stdio.h>
#include <string.h>

static double g_feed_rate = DEFAULT_FEED_RATE;
static double g_laser_power = 0.0;
static bool g_laser_enabled = false;
static bool g_absolute_mode = true;
static uint16_t g_seq = 1;
static uint64_t g_last_activity_ms = 0;
static double g_report_x = 0.0;
static double g_report_y = 0.0;

static void fill_cmd(motion_cmd_t *cmd, uint8_t type)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = type;
    if (g_seq == 0U) {
        g_seq = 1U;
    }
    cmd->seq = g_seq++;
    if (g_laser_enabled) {
        cmd->flags |= FLAG_LASER_ON;
    }
    if (g_absolute_mode) {
        cmd->flags |= FLAG_ABS_MODE;
    }
}

void gcode_processor_init(void)
{
    g_feed_rate = DEFAULT_FEED_RATE;
    g_laser_power = 0.0;
    g_laser_enabled = false;
    g_absolute_mode = true;
    g_seq = 1;
    g_last_activity_ms = 0;
    g_report_x = 0.0;
    g_report_y = 0.0;
}

void gcode_processor_build_emergency_stop(motion_cmd_t *out_cmd)
{
    if (out_cmd == NULL) {
        return;
    }
    fill_cmd(out_cmd, CMD_EMERGENCY_STOP);
    motion_cmd_set_crc(out_cmd);
    g_laser_enabled = false;
    g_laser_power = 0.0;
    g_last_activity_ms = uapi_systick_get_ms();
}

bool gcode_process_line(const char *line, int len, motion_cmd_t *out_cmds, int max_cmds, int *out_count)
{
    if (line == NULL || out_cmds == NULL || out_count == NULL || max_cmds <= 0) {
        return false;
    }

    gcode_line_t gc;
    gcode_init(&gc);
    for (int i = 0; i < len && i < GCODE_LINE_MAX - 1; i++) {
        gcode_add_char(&gc, line[i]);
    }
    if (!gcode_parse(&gc)) {
        *out_count = 0;
        return false;
    }

    int count = 0;
    if (gcode_has_word(&gc, 'F')) {
        g_feed_rate = gcode_get_value(&gc, 'F');
    }

    if (gcode_has_word(&gc, 'S')) {
        double s = gcode_get_value(&gc, 'S');
        if (s < 0) {
            s = 0;
        }
        if (s > LASER_S_MAX) {
            s = LASER_S_MAX;
        }
        g_laser_power = s;
        if (g_laser_enabled && count < max_cmds) {
            fill_cmd(&out_cmds[count], CMD_LASER_ON);
            out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
        }
    }

    if (gcode_has_word(&gc, 'M')) {
        int m_val = (int)gcode_get_value(&gc, 'M');
        if ((m_val == 3 || m_val == 4) && count < max_cmds) {
            g_laser_enabled = true;
            fill_cmd(&out_cmds[count], CMD_LASER_ON);
            out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
        } else if (m_val == 5 && count < max_cmds) {
            g_laser_enabled = false;
            fill_cmd(&out_cmds[count], CMD_LASER_OFF);
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
        }
    }

    if (gcode_has_word(&gc, 'G')) {
        int g_val = (int)gcode_get_value(&gc, 'G');
        if (g_val == 90) {
            g_absolute_mode = true;
        } else if (g_val == 91) {
            g_absolute_mode = false;
        } else if (g_val == 92 && count < max_cmds) {
            fill_cmd(&out_cmds[count], CMD_SET_ORIGIN);
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
            g_report_x = 0.0;
            g_report_y = 0.0;
        } else if (g_val == 0 || g_val == 1) {
            double target_x = g_report_x;
            double target_y = g_report_y;
            bool has_move = false;

            if (gcode_has_word(&gc, 'X')) {
                double val = gcode_get_value(&gc, 'X');
                target_x = g_absolute_mode ? val : (g_report_x + val);
                has_move = true;
            }
            if (gcode_has_word(&gc, 'Y')) {
                double val = gcode_get_value(&gc, 'Y');
                target_y = g_absolute_mode ? val : (g_report_y + val);
                has_move = true;
            }
            if (has_move && count < max_cmds) {
                fill_cmd(&out_cmds[count], (g_val == 0) ? CMD_G0_MOVE : CMD_G1_MOVE);
                out_cmds[count].target_x = (float)target_x;
                out_cmds[count].target_y = (float)target_y;
                out_cmds[count].feed_rate = (g_val == 0) ? (float)G0_FEED_RATE : (float)g_feed_rate;
                out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
                motion_cmd_set_crc(&out_cmds[count]);
                count++;
                g_report_x = target_x;
                g_report_y = target_y;
            }
        }
    }

    *out_count = count;
    if (count > 0) {
        g_last_activity_ms = uapi_systick_get_ms();
    }
    return count > 0;
}

bool grbl_process_dollar(const char *line, char *response, int resp_size)
{
    if (line == NULL || response == NULL || resp_size <= 0 || line[0] != '$') {
        return false;
    }
    if (strcmp(line, "$I") == 0) {
        snprintf(response, (size_t)resp_size, "[VER:1.1f.WS63_WIRELESS_TX:]\r\n[OPT:V,15,128]\r\nok\r\n");
    } else if (strcmp(line, "$G") == 0) {
        snprintf(response, (size_t)resp_size, "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n",
                 g_absolute_mode ? 90 : 91, g_laser_enabled ? 3 : 5, (int)g_feed_rate, (int)g_laser_power);
    } else if (strcmp(line, "$$") == 0) {
        snprintf(response, (size_t)resp_size,
                 "$0=10\r\n$1=25\r\n$10=1\r\n$30=%d\r\n$31=0\r\n$32=1\r\n$100=250.000\r\n$101=250.000\r\nok\r\n",
                 (int)LASER_S_MAX);
    } else if (strcmp(line, "$C") == 0) {
        snprintf(response, (size_t)resp_size, "[GC:G0 G54 G17 G21 G90 G94 M5]\r\nok\r\n");
    } else if (strcmp(line, "$X") == 0 || strcmp(line, "$H") == 0) {
        snprintf(response, (size_t)resp_size, "ok\r\n");
    } else if (strcmp(line, "$") == 0) {
        snprintf(response, (size_t)resp_size,
                 "$$ - View settings\r\n$G - View parser state\r\n$I - View build info\r\n$X - Kill alarm lock\r\nok\r\n");
    } else if (strcmp(line, "$D") == 0) {
        snprintf(response, (size_t)resp_size, "[MSG:wireless tx]\r\nok\r\n");
    } else {
        snprintf(response, (size_t)resp_size, "ok\r\n");
    }
    return true;
}

bool gcode_processor_is_idle(void)
{
    if (g_last_activity_ms == 0) {
        return true;
    }
    return (uapi_systick_get_ms() - g_last_activity_ms) > ACTIVITY_TIMEOUT_MS;
}

void gcode_processor_update_feedback_pos(double x, double y)
{
    g_report_x = x;
    g_report_y = y;
}

void gcode_processor_get_feedback_pos(double *x, double *y)
{
    if (x != NULL) {
        *x = g_report_x;
    }
    if (y != NULL) {
        *y = g_report_y;
    }
}

void grbl_format_status(char *buf, int size, double cur_x, double cur_y, int is_idle)
{
    snprintf(buf, (size_t)size, "<%s|MPos:%.3f,%.3f,0.000|FS:0,0>\r\n", is_idle ? "Idle" : "Run", cur_x, cur_y);
}
