 /*!
  * @file  motion_detection_sample.c
  * @brief  Example of radar detecting whether an object is moving
  * @copyright Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
  * @license The MIT License (MIT)
  * @author [Martin](Martin@dfrobot.com)
  * @version V1.0
  * @date 2025-09-29
  * @url https://github.com/dfrobot/DFRobot_C4001
  */

#include "pinctrl.h"
#include "common_def.h"
#include "soc_osal.h"
#include "gpio.h"
#include "systick.h"
#include "osal_debug.h"
#include "watchdog.h"
#include "app_init.h"
#include "uart.h"

#include "dfrobot_c4001.h"

static void radar_example(void)
{
    osal_printk("Radar example start!\r\n");

    DFRobot_C4001_INIT(CONFIG_RADAR_UART_BAUD, CONFIG_RADAR_UART_TX_PIN, CONFIG_RADAR_UART_RX_PIN, CONFIG_UART_BUS_ID);

    osal_printk("Radar connected!\r\n");

    // exist Mode
    setSensorMode(eExitMode);    

    sSensorStatus_t status = getStatus();
    osal_printk("work status  = %d\r\n", status.workStatus);   // 0 stop 1 start
    osal_printk("work mode    = %d\r\n", status.workMode);     // 0 exist 1 speed
    osal_printk("init status  = %d\r\n", status.initStatus);   // 0 no init 1 success

    // 设置检测范围
    if (setDetectionRange(30, 1000, 1000)) {
        osal_printk("set detection range successfully!\r\n");
    }


    // 设置触发灵敏度
    if (setTrigSensitivity(1)) {
        osal_printk("set trig sensitivity successfully!\r\n");
    }

    // 设置保持灵敏度
    if (setKeepSensitivity(2)) {
        osal_printk("set keep sensitivity successfully!\r\n");
    }

    // 设置触发延时和保持时间
    if (setDelay(100, 4)) {
        osal_printk("set delay successfully!\r\n");
    }

    uapi_watchdog_kick();

    // 设置 PWM 输出
    if (setPwm(50, 0, 10)) {
        osal_printk("set pwm period successfully!\r\n");
    }

    // 设置 IO 极性
    if (setIoPolaity(1)) {
        osal_printk("set Io Polaity successfully!\r\n");
    }

    // 打印当前参数
    osal_printk("trig sensitivity = %d\r\n", getTrigSensitivity());
    osal_printk("keep sensitivity = %d\r\n", getKeepSensitivity());
    osal_printk("min range = %d\r\n", getMinRange());
    osal_printk("max range = %d\r\n", getMaxRange());
    uapi_watchdog_kick();
    osal_printk("trig range = %d\r\n", getTrigRange());
    osal_printk("keep time = %d\r\n", getKeepTimerout());
    osal_printk("trig delay = %d\r\n", getTrigDelay());
    osal_printk("polaity = %d\r\n", getIoPolaity());
    uapi_watchdog_kick();
    sPwmData_t pwmData = getPwm();
    osal_printk("pwm1 = %d\r\n", pwmData.pwm1);
    osal_printk("pwm2 = %d\r\n", pwmData.pwm2);
    osal_printk("pwm timer = %d\r\n", pwmData.timer);

    // 主循环
    while (1) {
        uapi_watchdog_kick();
        if (motionDetection()) {
            osal_printk("exist motion\r\n");
        }
        uapi_systick_delay_ms(100);
    }
}

// 示例入口
static void radar_entry(void)
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)radar_example,
                                      NULL, "RadarTask", 0x2000);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, 25);
    }
    osal_kthread_unlock();
}

app_run(radar_entry);
