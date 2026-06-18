/**
 * @file safety_monitor.h
 * @brief 激光安全监控模块
 */
#ifndef SAFETY_MONITOR_H
#define SAFETY_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void safety_monitor_init(void);
void safety_update_last_sle_time(void);
void safety_update_last_cmd_time(void);
void safety_set_sle_connected(bool connected);
int task_safety_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_MONITOR_H */
