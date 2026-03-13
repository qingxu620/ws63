/**
 * @file gcode_processor.c
 * @brief G-Code 处理器实现
 *        将 Arduino .ino 中的 processGCode() 逻辑移植为纯 C
 *        输出 motion_cmd_t 命令包，由 SLE Client 发送
 */
#include "gcode_processor.h"
#include "gcode_parser.h"
#include "config.h"
#include "crc16.h"
#include "soc_osal.h"
#include "systick.h"
#include <stdio.h>
#include <string.h>

/* ================= 发射板本地状态 =================
 * 这些变量相当于发射板侧的软件“控制器上下文”。
 * 解析每一行 G-Code 时，会根据这组状态决定如何组帧。 */
static double g_feed_rate = 0.0;
static double g_laser_power = 0.0;
static bool g_laser_enabled = false;
static bool g_absolute_mode = true;
/* seq 用于接收板侧去重/应答匹配 */
static uint16_t g_seq = 1;
static unsigned long g_line_counter = 0;
static uint64_t g_last_activity_time = 0;

/* 来自接收板反馈的位置 (用于 '?' 状态报告) */
static double g_report_x = 0.0;
static double g_report_y = 0.0;

void gcode_processor_init(void)
{
    g_feed_rate = DEFAULT_FEED_RATE;
    g_laser_power = 0.0;
    g_laser_enabled = false;
    g_absolute_mode = true;
    /* 保留 seq=0 作为“尚无有效 ACK”的哨兵值，避免首条业务命令被误判已确认。 */
    g_seq = 1;
    g_line_counter = 0;
    g_last_activity_time = 0;
    g_report_x = 0.0;
    g_report_y = 0.0;
}

static void fill_cmd_header(motion_cmd_t *cmd, uint8_t type)
{
    /* 每条命令都从 0 初始化，避免脏数据进入 CRC */
    memset(cmd, 0, sizeof(motion_cmd_t));
    cmd->cmd = type;
    if (g_seq == 0U) {
        g_seq = 1U;
    }
    cmd->seq = g_seq++;
    cmd->flags = 0;
    if (g_laser_enabled)
        cmd->flags |= FLAG_LASER_ON;
    if (g_absolute_mode)
        cmd->flags |= FLAG_ABS_MODE;
}

bool gcode_process_line(const char *line, int len, motion_cmd_t *out_cmds, int max_cmds, int *out_count)
{
    gcode_line_t gc;
    gcode_init(&gc);

    /* 复制并解析 */
    for (int i = 0; i < len && i < GCODE_LINE_MAX - 1; i++) {
        gcode_add_char(&gc, line[i]);
    }
    if (!gcode_parse(&gc)) {
        *out_count = 0;
        return false;
    }

    int count = 0;

    /* 1) 提取 F (进给速度)
     * 仅更新本地状态，不单独下发命令，后续运动命令会带上最新 F。 */
    if (gcode_has_word(&gc, 'F')) {
        g_feed_rate = gcode_get_value(&gc, 'F');
    }

    /* 2) 提取 S (功率)
     * 仅写本地功率；若激光已开，补发 CMD_LASER_ON 实时改功率。 */
    if (gcode_has_word(&gc, 'S')) {
        double s = gcode_get_value(&gc, 'S');
        if (s < 0)
            s = 0;
        if (s > LASER_S_MAX)
            s = LASER_S_MAX;
        g_laser_power = s;

        /* 如果激光已开启，更新功率 */
        if (g_laser_enabled && count < max_cmds) {
            fill_cmd_header(&out_cmds[count], CMD_LASER_ON);
            out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
        }
    }

    /* 3) 处理 M 命令:
     * M3/M4 -> 开激光
     * M5    -> 关激光 */
    if (gcode_has_word(&gc, 'M')) {
        int m_val = (int)gcode_get_value(&gc, 'M');
        if ((m_val == 3 || m_val == 4) && count < max_cmds) {
            g_laser_enabled = true;
            fill_cmd_header(&out_cmds[count], CMD_LASER_ON);
            out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
        } else if (m_val == 5 && count < max_cmds) {
            g_laser_enabled = false;
            fill_cmd_header(&out_cmds[count], CMD_LASER_OFF);
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
        }
    }

    /* 4) 处理 G 命令:
     * G90/G91 切换绝对/相对模式
     * G92     设原点
     * G0/G1   生成运动命令 */
    if (gcode_has_word(&gc, 'G')) {
        int g_val = (int)gcode_get_value(&gc, 'G');

        if (g_val == 90) {
            g_absolute_mode = true;
        } else if (g_val == 91) {
            g_absolute_mode = false;
        } else if (g_val == 92 && count < max_cmds) {
            fill_cmd_header(&out_cmds[count], CMD_SET_ORIGIN);
            motion_cmd_set_crc(&out_cmds[count]);
            count++;
            g_report_x = 0;
            g_report_y = 0;
        } else if (g_val == 0 || g_val == 1) {
            /* G0/G1 运动命令 */
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
                uint8_t cmd_type = (g_val == 0) ? CMD_G0_MOVE : CMD_G1_MOVE;
                fill_cmd_header(&out_cmds[count], cmd_type);
                out_cmds[count].target_x = (float)target_x;
                out_cmds[count].target_y = (float)target_y;
                /* G0 使用固定快移速度；G1 使用当前进给速度 */
                out_cmds[count].feed_rate = (g_val == 0) ? (float)G0_FEED_RATE : (float)g_feed_rate;
                out_cmds[count].laser_pwr = (uint16_t)g_laser_power;
                motion_cmd_set_crc(&out_cmds[count]);
                count++;

                /* 发射板本地先行更新“预计位置”，收到接收板状态后会再修正 */
                g_report_x = target_x;
                g_report_y = target_y;
            }
        }
    }

    *out_count = count;

    /* 有效命令才计入活动，空行/仅参数更新不触发 Run 状态 */
    if (count > 0) {
        g_line_counter++;
        g_last_activity_time = uapi_systick_get_ms();
    }

    return count > 0;
}

/* ================= Grbl 兼容命令 ================= */

unsigned long gcode_processor_get_line_count(void)
{
    return g_line_counter;
}

bool gcode_processor_is_idle(void)
{
    /* 启动后若尚无业务命令，直接判空闲 */
    if (g_last_activity_time == 0)
        return true;
    uint64_t now = uapi_systick_get_ms();
    return (now - g_last_activity_time) > ACTIVITY_TIMEOUT_MS;
}

bool grbl_process_dollar(const char *line, char *response, int resp_size)
{
    if (line[0] != '$')
        return false;

    if (strcmp(line, "$I") == 0) {
        snprintf(response, resp_size, "[VER:1.1f.WS63:]\r\n[OPT:V,15,128]\r\nok\r\n");
    } else if (strcmp(line, "$G") == 0) {
        snprintf(response, resp_size, "[GC:G0 G54 G17 G21 G%d G94 M%d T0 F%d S%d]\r\nok\r\n", g_absolute_mode ? 90 : 91,
                 g_laser_enabled ? 3 : 5, (int)g_feed_rate, (int)g_laser_power);
    } else {
        snprintf(response, resp_size, "ok\r\n");
    }
    return true;
}

void grbl_format_status(char *buf, int size, double cur_x, double cur_y, double feed, int power, bool is_idle)
{
    const char *state = is_idle ? "Idle" : "Run";
    snprintf(buf, size, "<%s|MPos:%.3f,%.3f,0.000|FS:%d,%d>\r\n", state, cur_x, cur_y, (int)feed, power);
}

void gcode_processor_update_feedback_pos(double x, double y)
{
    /* 由 SLE 状态回传调用，覆盖本地预测位置 */
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
