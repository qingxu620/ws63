/**
 * @file interpolator.h
 * @brief 插补引擎 — 接收板核心模块
 *        从 Arduino performMove() 移植，使用 osal_udelay 实现微秒级精确延时
 */
#ifndef INTERPOLATOR_H
#define INTERPOLATOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  插补引擎初始化
 */
void interpolator_init(void);

/**
 * @brief  执行线性插补运动 (阻塞式)
 *         从当前位置移动到目标位置
 * @param  target_x      目标 X (mm)
 * @param  target_y      目标 Y (mm)
 * @param  feed_rate_mm_min  进给速度 (mm/min)
 */
void perform_move(double target_x, double target_y, double feed_rate_mm_min);

/**
 * @brief  获取当前 X 位置 (mm)
 */
double interpolator_get_x(void);

/**
 * @brief  获取当前 Y 位置 (mm)
 */
double interpolator_get_y(void);

/**
 * @brief  当前是否正在执行运动
 */
bool interpolator_is_busy(void);

/**
 * @brief  重置坐标原点
 */
void interpolator_set_origin(void);

/**
 * @brief  请求立即打断当前运动
 */
void interpolator_request_abort(void);

/**
 * @brief  插补任务入口函数 (由 RTOS 任务调用)
 */
int task_interpolator_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* INTERPOLATOR_H */
