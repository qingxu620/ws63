/**！
 * @file get_acceleration_sample.c
 * @brief Get the acceleration in the three directions of xyz, the range can be ±2g、±4g、±8g、±16g
 * @copyright  Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author [Martin](Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 * @url https://github.com/DFRobot/DFRobot_LIS
 */

#include "dfrobot_lis2dh12.h"

#define I2C_TASK_PRIO 24
#define I2C_TASK_STACK_SIZE 0x1000
#define DELAY_S 1000
#define DELAY_MS 1

static void get_acceleration_task(void)
{
    // Chip initialization
    while (!DFRobot_LIS2DH12_INIT(CONFIG_I2C_SLAVE_ADDR, CONFIG_I2C_SCL_MASTER_PIN, CONFIG_I2C_SDA_MASTER_PIN,
                                  CONFIG_I2C_MASTER_BUS_ID)) {
        uapi_watchdog_kick();
        osal_printk("Initialization failed, please check the connection and I2C address settings\r\n");
        uapi_systick_delay_ms(DELAY_S);
    }

    // Get chip id
    osal_printk("chip id : %X\r\n", getID());

    /**
      set range:Range(g)
                eLIS2DH12_2g,/< ±2g>/
                eLIS2DH12_4g,/< ±4g>/
                eLIS2DH12_8g,/< ±8g>/
                eLIS2DH12_16g,/< ±16g>/
    */
    setRange(eLIS2DH12_16g /*Range = */);

    /**
      Set data measurement rate：
        ePowerDown_0Hz
        eLowPower_1Hz
        eLowPower_10Hz
        eLowPower_25Hz
        eLowPower_50Hz
        eLowPower_100Hz
        eLowPower_200Hz
        eLowPower_400Hz
    */
    setAcquireRate(eLowPower_10Hz /*Rate = */);
    osal_printk("Acceleration:\r\n");
    uapi_systick_delay_ms(DELAY_S);

    while (1) {
        uapi_watchdog_kick();
        // Get the acceleration in the three directions of xyz
        long ax;
        long ay;
        long az;
        // The measurement range can be ±100g or ±200g set by the setRange() function
        ax = readAccX(); // Get the acceleration in the x direction
        ay = readAccY(); // Get the acceleration in the y direction
        az = readAccZ(); // Get the acceleration in the z direction
        // Print acceleration
        osal_printk("x: %d mg\t y: %d mg\t z: %d mg\r\n", ax, ay, az);

        uapi_systick_delay_ms(300 * DELAY_MS);
    }
}

static void get_acceleration_entry(void)
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle =
        osal_kthread_create((osal_kthread_handler)get_acceleration_task, 0, "getAccelerationTask", I2C_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, I2C_TASK_PRIO);
        osal_kfree(task_handle);
    }
    osal_kthread_unlock();
}

app_run(get_acceleration_entry);