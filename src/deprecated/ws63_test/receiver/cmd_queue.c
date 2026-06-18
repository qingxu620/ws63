/**
 * @file cmd_queue.c
 * @brief 线程安全环形命令队列实现
 *        使用 mutex 保护 + semaphore 通知的生产者-消费者模式
 */
#include "cmd_queue.h"
#include "soc_osal.h"
#include <string.h>

/* 队列数据结构 */
static motion_cmd_t g_queue_buf[CMD_QUEUE_SIZE];
static volatile uint8_t g_queue_head = 0;
static volatile uint8_t g_queue_tail = 0;

/* 同步原语 */
static osal_mutex g_queue_mutex;
static osal_semaphore g_queue_sem;

int cmd_queue_init(void)
{
    g_queue_head = 0;
    g_queue_tail = 0;
    memset(g_queue_buf, 0, sizeof(g_queue_buf));

    if (osal_mutex_init(&g_queue_mutex) != OSAL_SUCCESS) {
        return -1;
    }
    if (osal_sem_init(&g_queue_sem, 0) != OSAL_SUCCESS) {
        osal_mutex_destroy(&g_queue_mutex);
        return -1;
    }
    return 0;
}

bool cmd_queue_push(const motion_cmd_t *cmd)
{
    osal_mutex_lock(&g_queue_mutex);

    uint8_t next = (g_queue_head + 1) % CMD_QUEUE_SIZE;
    if (next == g_queue_tail) {
        /* 队列满 */
        osal_mutex_unlock(&g_queue_mutex);
        return false;
    }

    memcpy(&g_queue_buf[g_queue_head], cmd, sizeof(motion_cmd_t));
    g_queue_head = next;

    osal_mutex_unlock(&g_queue_mutex);
    osal_sem_up(&g_queue_sem); /* 通知消费者有数据 */
    return true;
}

bool cmd_queue_pop(motion_cmd_t *cmd)
{
    /* 阻塞等待信号:有命令可取 */
    if (osal_sem_down(&g_queue_sem) != OSAL_SUCCESS) {
        return false;
    }

    osal_mutex_lock(&g_queue_mutex);

    if (g_queue_head == g_queue_tail) {
        /* 理论上不会到这里，除非被 flush */
        osal_mutex_unlock(&g_queue_mutex);
        return false;
    }

    memcpy(cmd, &g_queue_buf[g_queue_tail], sizeof(motion_cmd_t));
    g_queue_tail = (g_queue_tail + 1) % CMD_QUEUE_SIZE;

    osal_mutex_unlock(&g_queue_mutex);
    return true;
}

uint8_t cmd_queue_free_count(void)
{
    osal_mutex_lock(&g_queue_mutex);
    uint8_t used;
    if (g_queue_head >= g_queue_tail) {
        used = g_queue_head - g_queue_tail;
    } else {
        used = CMD_QUEUE_SIZE - g_queue_tail + g_queue_head;
    }
    osal_mutex_unlock(&g_queue_mutex);
    return (uint8_t)(CMD_QUEUE_SIZE - 1 - used);
}

void cmd_queue_flush(void)
{
    osal_mutex_lock(&g_queue_mutex);
    g_queue_head = 0;
    g_queue_tail = 0;
    /* 重置信号量 (耗尽所有待处理的信号) */
    while (osal_sem_down_timeout(&g_queue_sem, 0) == OSAL_SUCCESS) {
        /* drain */
    }
    osal_mutex_unlock(&g_queue_mutex);
}
