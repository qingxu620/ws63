/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: Button Sample.
 */

#include "pinctrl.h"
#include "common_def.h"
#include "soc_osal.h"
#include "gpio.h"
#include "hal_gpio.h"
#include "watchdog.h"
#include "app_init.h"

// 只有在 Kconfig 开关打开时才编译整段代码
#ifdef CONFIG_SAMPLE_SUPPORT_MY_BUTTON

#define BUTTON_TASK_STACK_SIZE 0x1000
#define BUTTON_TASK_PRIO 17

/* 映射 Kconfig 变量及缺省值保护 */
#ifndef CONFIG_MY_BUTTON_PIN
#define CONFIG_MY_BUTTON_PIN 14
#endif

#ifndef CONFIG_MY_BUTTON_LED_PIN
#define CONFIG_MY_BUTTON_LED_PIN 2
#endif

static int g_ledState = 0;

/**
 * GPIO中断回调函数
 */
static void gpio_callback_func(pin_t pin, uintptr_t param)
{
    unused(pin);
    unused(param);
    g_ledState = !g_ledState;
    printf("Button pressed.\r\n");
}

/**
 * 任务主函数
 */
static void *button_task(const char *arg)
{
    unused(arg);

    // 设置LED引脚为GPIO输出模式
    uapi_pin_set_mode(CONFIG_MY_BUTTON_LED_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(CONFIG_MY_BUTTON_LED_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(CONFIG_MY_BUTTON_LED_PIN, GPIO_LEVEL_LOW);

    // 设置按键引脚为GPIO输入模式
    uapi_pin_set_mode(CONFIG_MY_BUTTON_PIN, HAL_PIO_FUNC_GPIO);
    gpio_select_core(CONFIG_MY_BUTTON_PIN, CORES_APPS_CORE);
    uapi_gpio_set_dir(CONFIG_MY_BUTTON_PIN, GPIO_DIRECTION_INPUT);

    // 注册中断回调（下降沿触发）
    errcode_t ret = uapi_gpio_register_isr_func(CONFIG_MY_BUTTON_PIN, GPIO_INTERRUPT_FALLING_EDGE, gpio_callback_func);
    if (ret != 0) {
        uapi_gpio_unregister_isr_func(CONFIG_MY_BUTTON_PIN);
    }

    printf("Button Sample Running on Pin %d, LED on Pin %d...\r\n", CONFIG_MY_BUTTON_PIN, CONFIG_MY_BUTTON_LED_PIN);

    while (1) {
        uapi_watchdog_kick(); // 喂狗，防止程序出现异常系统挂死
        if (g_ledState) {
            uapi_gpio_set_val(CONFIG_MY_BUTTON_LED_PIN, GPIO_LEVEL_HIGH);
        } else {
            uapi_gpio_set_val(CONFIG_MY_BUTTON_LED_PIN, GPIO_LEVEL_LOW);
        }
    }

    return NULL;
}

/**
 * 任务入口
 */
static void button_entry(void)
{
    uint32_t ret;
    osal_task *taskid;
    // 创建任务调度
    osal_kthread_lock();
    // 创建任务
    taskid = osal_kthread_create((osal_kthread_handler)button_task, NULL, "button_task", BUTTON_TASK_STACK_SIZE);
    ret = osal_kthread_set_priority(taskid, BUTTON_TASK_PRIO);
    if (ret != OSAL_SUCCESS) {
        printf("create button task failed.\n");
    }
    osal_kthread_unlock();
}

/* 注册应用入口 */
app_run(button_entry);

#endif /* CONFIG_SAMPLE_SUPPORT_MY_BUTTON */
