/**
 * @file cmd_queue.h
 * @brief 线程安全环形命令队列
 */
#ifndef CMD_QUEUE_H
#define CMD_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化命令队列
 * @return 0=成功, -1=失败
 */
int cmd_queue_init(void);

/**
 * @brief  压入一条命令 (task_sle_rx 调用)
 * @param  cmd 命令指针
 * @return true=成功, false=队列满
 */
bool cmd_queue_push(const motion_cmd_t *cmd);

/**
 * @brief  弹出一条命令 (task_interpolator 调用，阻塞等待)
 * @param  cmd 输出命令指针
 * @return true=成功
 */
bool cmd_queue_pop(motion_cmd_t *cmd);

/**
 * @brief  获取队列剩余空间
 * @return 剩余可用位置数
 */
uint8_t cmd_queue_free_count(void);

/**
 * @brief  清空命令队列
 */
void cmd_queue_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* CMD_QUEUE_H */
