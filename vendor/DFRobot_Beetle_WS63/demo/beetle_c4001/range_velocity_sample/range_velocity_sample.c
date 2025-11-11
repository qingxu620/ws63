 /*!
  * @file  range_velocity_sample.c
  * @brief  radar measurement demo
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

static void mRangeVelocity_task( void )
{

    osal_printk("Radar example start!\r\n");

    DFRobot_C4001_INIT(CONFIG_RADAR_UART_BAUD, CONFIG_RADAR_UART_TX_PIN, CONFIG_RADAR_UART_RX_PIN, CONFIG_UART_BUS_ID);

    osal_printk("Radar connected!\r\n");

    // speed Mode
    setSensorMode(eSpeedMode);

    sSensorStatus_t status = getStatus();
    osal_printk("work status  = %d\r\n", status.workStatus);   // 0 stop 1 start
    osal_printk("work mode    = %d\r\n", status.workMode);     // 0 exist 1 speed
    osal_printk("init status  = %d\r\n", status.initStatus);   // 0 no init 1 success
    uapi_watchdog_kick();

    /*
    * min Detection range Minimum distance, unit cm, range 0.3~20m (30~2000), not exceeding max, otherwise the function is abnormal.
    * max Detection range Maximum distance, unit cm, range 2.4~20m (240~2000)
    * thres Target detection threshold, dimensionless unit 0.1, range 0~6553.5 (0~65535)
    */
    if(setDetectThres(/*min*/11, /*max*/1200, /*thres*/10)) {
        osal_printk("set detect threshold successfully\r\n");
    }

    // set Fretting Detection
    setFrettingDetection(eON);

    uapi_watchdog_kick();

    osal_printk("min range = %d\r\n", getTMinRange());
    osal_printk("max range = %d\r\n", getTMaxRange());
    osal_printk("threshold range = %d\r\n", getThresRange());
    osal_printk("fretting detection = %d\r\n", getFrettingDetection());

    static char templine[32] = {0};
    while (1)
    {
        uapi_watchdog_kick();
        osal_printk("target number = %d\r\n", getTargetNumber());
        sprintf(templine, "target Speed = %.2f m/s\r\n", getTargetSpeed());
        osal_printk("%s", templine);
        sprintf(templine, "target distance = %.2f m\r\n", getTargetRange());
        osal_printk("%s", templine);
        uapi_systick_delay_ms(100);
    }

}

// 示例入口
static void mRangeVelocity_entry( void )
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)mRangeVelocity_task,
                                      NULL, "RadarTask", 0x2000);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, 25);
    }
    osal_kthread_unlock();
}

app_run(mRangeVelocity_entry);
