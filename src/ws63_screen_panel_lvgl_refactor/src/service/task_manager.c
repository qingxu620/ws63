/**
 * @file task_manager.c
 * @brief Minimal LiteOS task manager.
 */
#include "task_manager.h"
#include "soc_osal.h"

errcode_t task_manager_init(void)
{
    return ERRCODE_SUCC;
}

errcode_t task_create(const char *name, task_func_t func, void *arg,
                      uint32_t stack_size, uint32_t priority)
{
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(func, arg, name, stack_size);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[TASK] create '%s' failed\r\n", name);
        return ERRCODE_FAIL;
    }
    osal_kthread_set_priority(task, priority);
    osal_kfree(task);
    osal_kthread_unlock();
    return ERRCODE_SUCC;
}
