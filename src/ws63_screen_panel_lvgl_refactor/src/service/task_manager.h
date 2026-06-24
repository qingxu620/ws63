/**
 * @file task_manager.h
 * @brief Minimal task manager: creates panel_task only.
 */
#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include "errcode.h"

typedef int (*task_func_t)(void *arg);

errcode_t task_manager_init(void);
errcode_t task_create(const char *name, task_func_t func, void *arg,
                      uint32_t stack_size, uint32_t priority);

#endif
