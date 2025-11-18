/**！
 * @file beetle_urm37_sample.c
 * @brief Measuring distance by driving the URM37 via GPIO.
 * @copyright  Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author [Martin](Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 */

#include "pinctrl.h"
#include "common_def.h"
#include "soc_osal.h"
#include "gpio.h"
#include "systick.h"
#include "osal_debug.h"
#include "watchdog.h"
#include "app_init.h"

#define DELAY_US 1100
#define DELAY_MS 200

#define URM37_TASK_STACK_SIZE 0x1000
#define URM37_TASK_PRIO 24

void URM37_Init(void)
{

    uapi_pin_set_mode(CONFIG_URTRIG_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(CONFIG_URTRIG_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(CONFIG_URTRIG_PIN, GPIO_LEVEL_HIGH);

    uapi_pin_set_mode(CONFIG_URECHO_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(CONFIG_URECHO_PIN, GPIO_DIRECTION_INPUT);
}

unsigned int GetDistance(void)
{

    unsigned int flag = 0;
    static uint64_t start_time = 0, time = 0;
    unsigned int DistanceMeasured = 0;
    gpio_level_t value = 0;

    uapi_gpio_set_val(CONFIG_URTRIG_PIN, GPIO_LEVEL_LOW);
    uapi_systick_delay_us(DELAY_US);
    uapi_gpio_set_val(CONFIG_URTRIG_PIN, GPIO_LEVEL_HIGH);

    while (1) {

        uapi_watchdog_kick();

        value = uapi_gpio_get_output_val(CONFIG_URECHO_PIN);

        if (value == GPIO_LEVEL_LOW && flag == 0) {
            /*
             * 获取系统时间
             * get SysTime
             */
            start_time = uapi_systick_get_us();
            flag = 1;
        }

        if (value == GPIO_LEVEL_HIGH && flag == 1) {
            /*
             * 获取高电平持续时间
             * Get high level duration
             */
            time = uapi_systick_get_us() - start_time;
            break;
        }
    }

    if (time >= 50000) // the reading is invalid.
    {
        printf("Invalid");
    } else {
        DistanceMeasured = time / 50; // every 50us low level stands for 1cm
        printf("distance = %ucm\n", DistanceMeasured);
    }

    return DistanceMeasured;
}

void URM37Task(void)
{
    URM37_Init();
    printf("URM37Task init\r\n");

    while (1) {

        GetDistance();
        osal_mdelay(DELAY_MS);
    }
}

void URM37Entry(void)
{
    uint32_t ret;
    osal_task *taskid;
    // 创建任务调度
    osal_kthread_lock();
    // 创建任务1
    taskid = osal_kthread_create((osal_kthread_handler)URM37Task, NULL, "URM37Task", URM37_TASK_STACK_SIZE);
    ret = osal_kthread_set_priority(taskid, URM37_TASK_PRIO);
    if (ret != OSAL_SUCCESS) {
        printf("create task1 failed .\n");
    }
    osal_kthread_unlock();
}
app_run(URM37Entry);