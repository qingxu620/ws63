/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: PWM Sample Source. \n
 *
 * History: \n
 * 2023-06-27, Create file. \n
 */
#if defined(CONFIG_PWM_SUPPORT_LPM)
#include "pm_veto.h"
#endif
#include "common_def.h"
#include "pinctrl.h"
#include "pwm.h"
#include "tcxo.h"
#include "soc_osal.h"
#include "app_init.h"

#define TEST_TCXO_DELAY_500MS      500
#define PWM_LOW_TIME_CYC           10
#define PWM_HIGH_TIME_CYC          10
#define PWM_TASK_PRIO              24
#define PWM_TASK_STACK_SIZE        0x1000

static uint32_t g_pwm_cyc_done_cnt = 0;
static errcode_t pwm_sample_callback(uint8_t channel)
{
    unused(channel);
    g_pwm_cyc_done_cnt++;
    return ERRCODE_SUCC;
}

/*
void pwm_repeat_mode(void)
{
    pwm_config_t cfg_repeat = {
        PWM_LOW_TIME_CYC,
        PWM_HIGH_TIME_CYC,
        0,
        0xFF,
        true
    };

    uapi_pwm_init();
    uapi_pwm_open(CONFIG_PWM_CHANNEL, &cfg_repeat);
    uapi_pwm_register_interrupt(CONFIG_PWM_CHANNEL, pwm_sample_callback);
#if defined(CONFIG_PWM_SUPPORT_LPM)
    // veto sleep, or pwm will stop after sleep
    uapi_pm_add_sleep_veto(PM_USER0_VETO_ID);
#endif
#ifdef CONFIG_PWM_USING_V151
    uint8_t channel_id = CONFIG_PWM_CHANNEL;
    uapi_pwm_set_group(CONFIG_PWM_GROUP_ID, &channel_id, 1);
    uapi_pwm_start_group(CONFIG_PWM_GROUP_ID);
#else
    uapi_pwm_start(CONFIG_PWM_CHANNEL);
#endif
    uapi_tcxo_delay_ms((uint32_t)TEST_TCXO_DELAY_500MS);

#ifdef CONFIG_PWM_USING_V151
    // update duty ratio config
    while (cfg_repeat.low_time <= PWM_HIGH_TIME_CYC) {
        uapi_pwm_update_cfg(CONFIG_PWM_CHANNEL, &cfg_repeat);
        uapi_tcxo_delay_ms((uint32_t)TEST_TCXO_DELAY_500MS);
        osal_printk("pwm cyc_done_cnt:%u, duty_radio:[%u/%u]\r\n", g_pwm_cyc_done_cnt,
            cfg_repeat.high_time, cfg_repeat.high_time + cfg_repeat.low_time);
        cfg_repeat.low_time++;
        cfg_repeat.high_time--;
    }
    uapi_pwm_stop_group(CONFIG_PWM_GROUP_ID);
#endif

#if defined(CONFIG_PWM_SUPPORT_LPM)
    uapi_pm_remove_sleep_veto(PM_USER0_VETO_ID);  // remove veto sleep
#endif
    uapi_pwm_close(CONFIG_PWM_CHANNEL);
    uapi_pwm_deinit();
}
*/

static void *pwm_task(const char *arg)
{
    UNUSED(arg);
    pwm_config_t cfg_repeat = {
        PWM_LOW_TIME_CYC,
        PWM_HIGH_TIME_CYC,
        0,
        0xFF,
        true
    };

    uapi_pin_set_mode(CONFIG_PWM_PIN, CONFIG_PWM_PIN_MODE);
    uapi_pwm_init();
    uapi_pwm_open(CONFIG_PWM_CHANNEL, &cfg_repeat);
    uapi_pwm_register_interrupt(CONFIG_PWM_CHANNEL, pwm_sample_callback);
#ifdef CONFIG_PWM_USING_V151
    uint8_t channel_id = CONFIG_PWM_CHANNEL;
    uapi_pwm_set_group(CONFIG_PWM_GROUP_ID, &channel_id, 1);
    uapi_pwm_start_group(CONFIG_PWM_GROUP_ID);
#else
    uapi_pwm_start(CONFIG_PWM_CHANNEL);
#endif

    osal_printk("PWM2 output started: 50%% duty cycle\r\n");

    while (1) {
        osal_msleep(1000);
    }

    return NULL;
}

static void pwm_entry(void)
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)pwm_task, 0, "PwmTask", PWM_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, PWM_TASK_PRIO);
    }
    osal_kthread_unlock();
}

/* Run the pwm_entry. */
app_run(pwm_entry);