/**
 * @file gcode_processor.c
 * @brief Arduino-style G-code semantic layer for the single-board sample.
 */
#include "legacy_wifi_gcode_processor.h"
#include "legacy_wifi_config.h"
#include "legacy_wifi_gcode_parser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static double g_feed_rate = LEGACY_WIFI_DEFAULT_FEED_RATE;
static double g_laser_power = 0.0;
static bool g_laser_enabled = false;
static bool g_absolute_mode = true;
static int g_motion_mode = 0;
static uint16_t g_seq = 1;
static unsigned long g_line_counter = 0;
static double g_plan_x = 0.0;
static double g_plan_y = 0.0;

static void fill_cmd_header(legacy_wifi_motion_cmd_t *cmd, uint8_t type)
{
    memset(cmd, 0, sizeof(legacy_wifi_motion_cmd_t));
    cmd->cmd = type;
    cmd->seq = g_seq++;
    if (g_laser_enabled) {
        cmd->flags |= LEGACY_WIFI_FLAG_LASER_ON;
    }
    if (g_absolute_mode) {
        cmd->flags |= LEGACY_WIFI_FLAG_ABS_MODE;
    }
}

static bool append_laser_off(legacy_wifi_motion_cmd_t *out_cmds, int max_cmds, int *count)
{
    if (out_cmds == NULL || count == NULL || *count >= max_cmds) {
        return false;
    }

    fill_cmd_header(&out_cmds[*count], LEGACY_WIFI_CMD_LASER_OFF);
    out_cmds[*count].flags = 0;
    out_cmds[*count].laser_pwr = 0;
    (*count)++;
    return true;
}

void legacy_wifi_gcode_processor_init(void)
{
    g_feed_rate = LEGACY_WIFI_DEFAULT_FEED_RATE;
    g_laser_power = 0.0;
    g_laser_enabled = false;
    g_absolute_mode = true;
    g_motion_mode = 0;
    g_seq = 1;
    g_line_counter = 0;
    legacy_wifi_gcode_processor_set_origin();
}

void legacy_wifi_gcode_processor_set_origin(void)
{
    g_plan_x = 0.0;
    g_plan_y = 0.0;
}

void legacy_wifi_gcode_processor_build_emergency_stop(legacy_wifi_motion_cmd_t *out_cmd)
{
    if (out_cmd == NULL) {
        return;
    }
    fill_cmd_header(out_cmd, LEGACY_WIFI_CMD_EMERGENCY_STOP);
    out_cmd->laser_pwr = 0;
    g_laser_enabled = false;
    g_laser_power = 0.0;
}

static bool is_word_start(const legacy_wifi_gcode_line_t *gc, int pos, char letter)
{
    return gc->line[pos] == letter && (pos == 0 || !isalpha((unsigned char)gc->line[pos - 1]));
}

bool legacy_wifi_gcode_process_line(const char *line, int len, legacy_wifi_motion_cmd_t *out_cmds, int max_cmds, int *out_count)
{
    legacy_wifi_gcode_line_t gc;
    int count = 0;
    bool suppress_modal_motion = false;
    bool laser_off_requested = false;
    bool line_processed = false;

    if (out_count == NULL || out_cmds == NULL || max_cmds <= 0) {
        return false;
    }
    *out_count = 0;

    legacy_wifi_gcode_init(&gc);
    for (int i = 0; i < len && i < LEGACY_WIFI_GCODE_LINE_MAX - 1; i++) {
        legacy_wifi_gcode_add_char(&gc, line[i]);
    }
    if (!legacy_wifi_gcode_parse(&gc)) {
        return false;
    }

    if (legacy_wifi_gcode_has_word(&gc, 'F')) {
        double f = legacy_wifi_gcode_get_value(&gc, 'F');
        if (f > 0.0) {
            g_feed_rate = f;
            line_processed = true;
        }
    }

    if (legacy_wifi_gcode_has_word(&gc, 'S')) {
        double s = legacy_wifi_gcode_get_value(&gc, 'S');
        if (s < 0.0) {
            s = 0.0;
        }
        if (s > LEGACY_WIFI_LASER_S_MAX) {
            s = LEGACY_WIFI_LASER_S_MAX;
        }
        g_laser_power = s;
        line_processed = true;
        if (s <= 0.0) {
            laser_off_requested = true;
        }
    }

    if (legacy_wifi_gcode_has_word(&gc, 'M')) {
        int m_val = (int)legacy_wifi_gcode_get_value(&gc, 'M');
        if (m_val == 3 || m_val == 4) {
            g_laser_enabled = true;
        } else if (m_val == 5) {
            g_laser_enabled = false;
            laser_off_requested = true;
        }
        line_processed = true;
    }

    if (laser_off_requested && !append_laser_off(out_cmds, max_cmds, &count)) {
        *out_count = count;
        return count > 0;
    }

    for (int i = 0; i < gc.len; i++) {
        if (!is_word_start(&gc, i, 'G')) {
            continue;
        }

        int g_val = (int)strtod(&gc.line[i + 1], NULL);
        if (g_val == 90) {
            g_absolute_mode = true;
            line_processed = true;
        } else if (g_val == 91) {
            g_absolute_mode = false;
            line_processed = true;
        } else if (g_val == 0 || g_val == 1 || g_val == 2 || g_val == 3) {
            g_motion_mode = g_val;
            line_processed = true;
        } else if ((g_val == 28 || g_val == 92) && count < max_cmds) {
            fill_cmd_header(&out_cmds[count], LEGACY_WIFI_CMD_SET_ORIGIN);
            count++;
            legacy_wifi_gcode_processor_set_origin();
            suppress_modal_motion = true;
            line_processed = true;
        }
    }

    if (!suppress_modal_motion && (legacy_wifi_gcode_has_word(&gc, 'X') || legacy_wifi_gcode_has_word(&gc, 'Y')) && count < max_cmds) {
        double target_x = g_plan_x;
        double target_y = g_plan_y;

        if (legacy_wifi_gcode_has_word(&gc, 'X')) {
            double x = legacy_wifi_gcode_get_value(&gc, 'X');
            target_x = g_absolute_mode ? x : (g_plan_x + x);
        }
        if (legacy_wifi_gcode_has_word(&gc, 'Y')) {
            double y = legacy_wifi_gcode_get_value(&gc, 'Y');
            target_y = g_absolute_mode ? y : (g_plan_y + y);
        }

        fill_cmd_header(&out_cmds[count], (g_motion_mode == 0) ? LEGACY_WIFI_CMD_G0_MOVE : LEGACY_WIFI_CMD_G1_MOVE);
        out_cmds[count].target_x = (float)target_x;
        out_cmds[count].target_y = (float)target_y;
        out_cmds[count].feed_rate = (g_motion_mode == 0) ? (float)LEGACY_WIFI_G0_FEED_RATE : (float)g_feed_rate;
        out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
        count++;
        g_plan_x = target_x;
        g_plan_y = target_y;
    }

    *out_count = count;
    if (count > 0) {
        g_line_counter++;
    }
    return count > 0 || line_processed;
}

unsigned long legacy_wifi_gcode_processor_get_line_count(void)
{
    return g_line_counter;
}

double legacy_wifi_gcode_processor_get_feed_rate(void)
{
    return g_feed_rate;
}

double legacy_wifi_gcode_processor_get_laser_power(void)
{
    return g_laser_power;
}

bool legacy_wifi_gcode_processor_laser_is_enabled(void)
{
    return g_laser_enabled;
}

bool legacy_wifi_gcode_processor_is_absolute_mode(void)
{
    return g_absolute_mode;
}
