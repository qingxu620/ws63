/**
 * @file gcode_processor.c
 * @brief Arduino-style G-code semantic layer for the single-board sample.
 */
#include "gcode_processor.h"
#include "config.h"
#include "gcode_parser.h"
#include <string.h>

static double g_feed_rate = DEFAULT_FEED_RATE;
static double g_laser_power = 0.0;
static bool g_laser_enabled = false;
static bool g_absolute_mode = true;
static uint16_t g_seq = 1;
static unsigned long g_line_counter = 0;
static double g_report_x = 0.0;
static double g_report_y = 0.0;

static void fill_cmd_header(motion_cmd_t *cmd, uint8_t type)
{
    memset(cmd, 0, sizeof(motion_cmd_t));
    cmd->cmd = type;
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
    g_line_counter = 0;
    g_report_x = 0.0;
    g_report_y = 0.0;
}

void gcode_processor_build_emergency_stop(motion_cmd_t *out_cmd)
{
    if (out_cmd == NULL) {
        return;
    }
    fill_cmd_header(out_cmd, CMD_EMERGENCY_STOP);
    out_cmd->laser_pwr = 0;
    g_laser_enabled = false;
    g_laser_power = 0.0;
}

bool gcode_process_line(const char *line, int len, motion_cmd_t *out_cmds, int max_cmds, int *out_count)
{
    gcode_line_t gc;
    int count = 0;

    if (out_count == NULL || out_cmds == NULL || max_cmds <= 0) {
        return false;
    }
    *out_count = 0;

    gcode_init(&gc);
    for (int i = 0; i < len && i < GCODE_LINE_MAX - 1; i++) {
        gcode_add_char(&gc, line[i]);
    }
    if (!gcode_parse(&gc)) {
        return false;
    }

    if (gcode_has_word(&gc, 'F')) {
        double f = gcode_get_value(&gc, 'F');
        if (f > 0.0) {
            g_feed_rate = f;
        }
    }

    if (gcode_has_word(&gc, 'S')) {
        double s = gcode_get_value(&gc, 'S');
        if (s < 0.0) {
            s = 0.0;
        }
        if (s > LASER_S_MAX) {
            s = LASER_S_MAX;
        }
        g_laser_power = s;
    }

    if (gcode_has_word(&gc, 'M')) {
        int m_val = (int)gcode_get_value(&gc, 'M');
        if (m_val == 3 || m_val == 4) {
            g_laser_enabled = true;
        } else if (m_val == 5 && count < max_cmds) {
            g_laser_enabled = false;
            fill_cmd_header(&out_cmds[count], CMD_LASER_OFF);
            count++;
        }
    }

    if (gcode_has_word(&gc, 'G')) {
        int g_val = (int)gcode_get_value(&gc, 'G');

        if (g_val == 90) {
            g_absolute_mode = true;
        } else if (g_val == 91) {
            g_absolute_mode = false;
        } else if ((g_val == 28 || g_val == 92) && count < max_cmds) {
            fill_cmd_header(&out_cmds[count], CMD_SET_ORIGIN);
            count++;
            g_report_x = 0.0;
            g_report_y = 0.0;
        } else if ((g_val == 0 || g_val == 1 || g_val == 2 || g_val == 3) && count < max_cmds) {
            double target_x = g_report_x;
            double target_y = g_report_y;
            bool has_move = false;

            if (gcode_has_word(&gc, 'X')) {
                double x = gcode_get_value(&gc, 'X');
                target_x = g_absolute_mode ? x : (g_report_x + x);
                has_move = true;
            }
            if (gcode_has_word(&gc, 'Y')) {
                double y = gcode_get_value(&gc, 'Y');
                target_y = g_absolute_mode ? y : (g_report_y + y);
                has_move = true;
            }

            if (has_move) {
                fill_cmd_header(&out_cmds[count], (g_val == 0) ? CMD_G0_MOVE : CMD_G1_MOVE);
                out_cmds[count].target_x = (float)target_x;
                out_cmds[count].target_y = (float)target_y;
                out_cmds[count].feed_rate = (g_val == 0) ? (float)G0_FEED_RATE : (float)g_feed_rate;
                out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
                count++;
                g_report_x = target_x;
                g_report_y = target_y;
            }
        }
    }

    *out_count = count;
    if (count > 0) {
        g_line_counter++;
    }
    return count > 0;
}

void gcode_processor_note_executed(double x, double y)
{
    g_report_x = x;
    g_report_y = y;
}

unsigned long gcode_processor_get_line_count(void)
{
    return g_line_counter;
}

double gcode_processor_get_feed_rate(void)
{
    return g_feed_rate;
}

double gcode_processor_get_laser_power(void)
{
    return g_laser_power;
}

bool gcode_processor_laser_is_enabled(void)
{
    return g_laser_enabled;
}

bool gcode_processor_is_absolute_mode(void)
{
    return g_absolute_mode;
}
