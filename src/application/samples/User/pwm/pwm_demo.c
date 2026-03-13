/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: PWM Breathing Light Sample.
 */

#include "common_def.h"
#include "pinctrl.h"
#include "pwm.h"
#include "tcxo.h"
#include "soc_osal.h"
#include "app_init.h"
#include <math.h>

// 只有在 Kconfig 开关打开时才编译整段代码
#ifdef CONFIG_SAMPLE_SUPPORT_MY_PWM

#if defined(CONFIG_PWM_SUPPORT_LPM)
#include "pm_veto.h"
#endif

/* 呼吸灯常量定义 */
#define PWM_TOTAL_PERIOD           2000    // 总周期计数值
#define BREATH_UPDATE_MS           20      // 刷新频率 20ms
#define PI                         3.14159265f
#define PWM_TASK_PRIO              24
#define PWM_TASK_STACK_SIZE        0x1000

/* 映射 Kconfig 变量及缺省值保护 */
#ifndef CONFIG_MY_PWM_PIN
#define CONFIG_MY_PWM_PIN          2
#endif

#ifndef CONFIG_PWM_PIN_MODE
#define CONFIG_PWM_PIN_MODE        1
#endif

#ifndef CONFIG_PWM_CHANNEL
#define CONFIG_PWM_CHANNEL         2
#endif

#ifndef CONFIG_PWM_GROUP_ID
#define CONFIG_PWM_GROUP_ID        0
#endif

/**
 * 核心业务逻辑：正弦波呼吸
 */
static void pwm_breathing_logic(void)
{
    float angle = 0.0f;
    float step = 0.05f; // 呼吸速度

    // 初始化配置 (根据你的 SDK 结构顺序：low, high, offset, cycles, repeat)
    pwm_config_t cfg = {
        PWM_TOTAL_PERIOD, // low_time
        0,                // high_time
        0,                // offset_time
        0,                // cycles
        true              // repeat
    };

    uapi_pwm_init();
    uapi_pwm_open(CONFIG_PWM_CHANNEL, &cfg);

#if defined(CONFIG_PWM_SUPPORT_LPM)
    uapi_pm_add_sleep_veto(PM_USER0_VETO_ID); // 阻止休眠
#endif

    // 启动 PWM（针对 V151 版本使用组启动，否则使用标准启动）
#ifdef CONFIG_PWM_USING_V151
    uint8_t ch = CONFIG_PWM_CHANNEL;
    uapi_pwm_set_group(CONFIG_PWM_GROUP_ID, &ch, 1);
    uapi_pwm_start_group(CONFIG_PWM_GROUP_ID);
#else
    uapi_pwm_start(CONFIG_PWM_CHANNEL);
#endif

    osal_printk("Breath Light Running on Pin %d...\r\n", CONFIG_MY_PWM_PIN);

    while (1) {
        // 1. 正弦波计算：将 [-1, 1] 映射到 [0, 1]
        float sin_val = (sinf(angle) + 1.0f) / 2.0f;
        
        // 2. 映射到高低电平时间
        uint32_t high = (uint32_t)(sin_val * (float)PWM_TOTAL_PERIOD);
        if (high == 0) high = 1;
        if (high >= PWM_TOTAL_PERIOD) high = PWM_TOTAL_PERIOD - 1;
        uint32_t low = PWM_TOTAL_PERIOD - high;

        // 3. 更新配置并同步到硬件
        cfg.high_time = high;
        cfg.low_time = low;
        uapi_pwm_update_cfg(CONFIG_PWM_CHANNEL, &cfg);

        // 4. 角度累加
        angle += step;
        if (angle >= 2.0f * PI) angle = 0.0f;

        // 5. 任务级延时
        osal_msleep(BREATH_UPDATE_MS);
    }
}

/**
 * 任务主函数
 */
static void *pwm_task(const char *arg)
{
    UNUSED(arg);
    
    // 设置引脚复用
    uapi_pin_set_mode(CONFIG_MY_PWM_PIN, CONFIG_PWM_PIN_MODE);

    // 执行呼吸逻辑
    pwm_breathing_logic();

    return NULL;
}

/**
 * 任务入口
 */
static void pwm_entry(void)
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)pwm_task, 0, "PwmBreathTask", PWM_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, PWM_TASK_PRIO);
    }
    osal_kthread_unlock();
}

/* 注册应用入口 */
app_run(pwm_entry);

#endif /* CONFIG_SAMPLE_SUPPORT_MY_PWM */