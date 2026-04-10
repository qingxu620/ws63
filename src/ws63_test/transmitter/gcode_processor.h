/**
 * @file gcode_processor.h
 * @brief G-Code 处理器 — 将解析后的 G-Code 转换为 motion_cmd_t 命令包
 */
#ifndef GCODE_PROCESSOR_H
#define GCODE_PROCESSOR_H

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 G-Code 处理器
 */
void gcode_processor_init(void);

/**
 * @brief  处理一行 G-Code 文本
 * @param  line  G-Code 文本行
 * @param  len   文本长度
 * @param  out_cmds  输出命令数组 (最多 4 条，顺序即发送顺序)
 * @param  out_count 输出实际命令数
 * @return true=成功生成命令, false=无需发送命令
 */
bool gcode_process_line(const char *line, int len, motion_cmd_t *out_cmds, int max_cmds, int *out_count);

/**
 * @brief  处理 Grbl '$' 命令
 * @param  line  命令文本
 * @param  response 输出回复文本
 * @param  resp_size 回复缓冲大小
 * @return true=是$命令并已处理
 * @note   当前额外支持 `$CAP?`，便于比赛演示时快速查看作品能力画像
 */
bool grbl_process_dollar(const char *line, char *response, int resp_size);

/**
 * @brief  获取已处理 G-Code 行数 (等效 Arduino lineCounter)
 */
unsigned long gcode_processor_get_line_count(void);

/**
 * @brief  判断是否空闲 (超过 ACTIVITY_TIMEOUT_MS 无命令)
 * @note   该状态用于 '?' 查询返回 Idle/Run
 */
bool gcode_processor_is_idle(void);

/**
 * @brief  生成 Grbl 状态报告字符串 ('?' 命令回复)
 * @param  buf    输出缓冲
 * @param  size   缓冲大小
 * @param  cur_x  当前 X (从接收板反馈)
 * @param  cur_y  当前 Y
 * @param  feed   当前进给速度
 * @param  power  当前激光功率
 * @param  is_idle 是否空闲
 */
void grbl_format_status(char *buf, int size, double cur_x, double cur_y, double feed, int power, bool is_idle);

/**
 * @brief  更新来自接收板的坐标反馈
 * @param  x 当前 X
 * @param  y 当前 Y
 */
void gcode_processor_update_feedback_pos(double x, double y);

/**
 * @brief  获取当前反馈坐标
 * @param  x 输出 X 指针，可为 NULL
 * @param  y 输出 Y 指针，可为 NULL
 */
void gcode_processor_get_feedback_pos(double *x, double *y);

#ifdef __cplusplus
}
#endif

#endif /* GCODE_PROCESSOR_H */
