/**
 * @file main.c
 * @brief WS63 安全终端节点主入口（第一版：SLE + LED_ON / LED_OFF）
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "soc_osal.h"

#include "safety_service.h"
#include "safety_sle_server.h"

static int safety_sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    if (safety_sle_server_init() != ERRCODE_SUCC) {
        osal_printk("[safety node] safety sle init failed\r\n");
    }
    return OSAL_SUCCESS;
}

static int safety_node_task(void *arg)
{
    safety_service_state_t state = {0};
    errcode_t ret;
    uint32_t loop_count = 0;

    unused(arg);

    while (1) {
        ret = safety_service_get_state(&state);
        if (ret == ERRCODE_SUCC) {
            (void)safety_sle_server_publish_state(&state);
            if ((loop_count % 5U) == 0U) {
                osal_printk("[safety node] led_ready=%u led_on=%u state=%u err=%u conn=%u\r\n",
                            state.led_ready ? 1U : 0U, state.led_on ? 1U : 0U,
                            state.status_code, state.error_code, safety_sle_server_get_conn_id());
            }
        } else {
            osal_printk("[safety node] get state failed: 0x%x\r\n", ret);
        }

        loop_count++;
        osal_msleep(SAFETY_NODE_POLL_INTERVAL_MS);
    }

    return OSAL_SUCCESS;
}

static void safety_node_entry(void)
{
    safety_service_config_t config = {
        .led_pin = SAFETY_NODE_LED_PIN,
        .led_active_high = (SAFETY_NODE_LED_ACTIVE_HIGH != 0),
        .boot_led_on = (SAFETY_NODE_BOOT_LED_ON != 0),
    };
    osal_task *task = NULL;
    errcode_t ret;

    osal_printk("========================================\r\n");
    osal_printk("  WS63 Safety Node Board\r\n");
    osal_printk("========================================\r\n");
    osal_printk("[safety node] current validation path: SLE -> LED_ON / LED_OFF -> GPIO%u\r\n",
                (unsigned int)config.led_pin);

    ret = safety_service_init(&config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[safety node] init failed: 0x%x\r\n", ret);
        return;
    }

    osal_kthread_lock();
    task = osal_kthread_create(safety_node_task, NULL, "safety_node", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[safety node] create task failed\r\n");
        return;
    }
    osal_kthread_set_priority(task, TASK_PRIO_DEFAULT);
    osal_kfree(task);

    task = osal_kthread_create(safety_sle_init_task, NULL, "safety_sle", TASK_STACK_SIZE_SLE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[safety node] create safety_sle task failed\r\n");
        return;
    }
    osal_kthread_set_priority(task, TASK_PRIO_SLE);
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[safety node] task created\r\n");
}

app_run(safety_node_entry);
