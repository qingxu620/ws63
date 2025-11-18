/**！
 * @file interrupt_sample.c
 * @brief Interrupt detection
 * @n In this example, the enable eZHigherThanTh interrupt event means when the acceleration in the Z direction exceeds
 * the
 * @n threshold set by the program, the interrupt level can be detected on the interrupt pin int1/int2 we set, and the
 * level change on the
 * @n interrupt pin can be used to determine whether the interrupt occurs. The following are the 6 settable interrupt
 * events：eXHigherThanTh,
 * @n eXLowerThanTh, eYHigherThanTh, eYLowerThanTh, eZHigherThanTh, eZLowerThanTh. For a detailed explanation of each of
 * them,
 * @n please look up the comments of the enableInterruptEvent() function.
 * @n This example needs to connect the int2/int1 pin of the module to the interrupt pin of the motherboard.
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
#define THRESHOLD 6

// Interrupt generation flag
volatile bool intFlag = false;

void interEvent(pin_t pin, uintptr_t param)
{
    UNUSED(pin);
    UNUSED(param);
    intFlag = true;
}

// from Arduino
#define RISING 0x00000001
#define FALLING 0x00000002
#define CHANGE 0x00000003
#define ONLOW 0x00000004
#define ONHIGH 0x00000005

void attachInterrupt(uint8_t pin, gpio_callback_t callback, uint32_t mode)
{
    uapi_pin_set_mode(pin, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
    errcode_t ret = uapi_gpio_register_isr_func(pin, mode, callback);
    if (ret != 0) {
        uapi_gpio_unregister_isr_func(pin);
    }
}

static void interrupt_task(void)
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

    attachInterrupt(CONFIG_INTERRUPT_PIN /*Interrupt No*/, interEvent, CHANGE);

    /**
    Set the threshold of interrupt source 1 interrupt
    threshold:Threshold(g)
    */
    setInt1Th(THRESHOLD); // Unit: g

    /*!
    Enable interrupt
    Interrupt pin selection:
      eINT1 = 0,/<int1 >/
      eINT2,/<int2>/
    Interrupt event selection:
      eXLowerThanTh ,/<The acceleration in the x direction is less than the threshold>/
      eXHigherThanTh ,/<The acceleration in the x direction is greater than the threshold>/
      eYLowerThanTh,/<The acceleration in the y direction is less than the threshold>/
      eYHigherThanTh,/<The acceleration in the y direction is greater than the threshold>/
      eZLowerThanTh,/<The acceleration in the z direction is less than the threshold>/
      eZHigherThanTh,/<The acceleration in the z direction is greater than the threshold>/
    */
    enableInterruptEvent(eINT1 /*int pin*/, eZHigherThanTh /*interrupt event = */);

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
        // The interrupt flag is set
        if (intFlag == true) {
            // Check whether the interrupt event is generated in interrupt 1
            if (getInt1Event(eZHigherThanTh)) {
                osal_printk("The acceleration in the z direction is greater than the threshold\r\n");
            }
            intFlag = false;
        }
    }
}

static void interrupt_entry(void)
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)interrupt_task, 0, "interruptTask", I2C_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, I2C_TASK_PRIO);
        osal_kfree(task_handle);
    }
    osal_kthread_unlock();
}

app_run(interrupt_entry);