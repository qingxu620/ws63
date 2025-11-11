/*!
 * @file bme680_i2c_sample.ino
 * @brief connect bme680 I2C interface with your board (please reference board compatibility)
 * @n Temprature, Humidity, pressure, altitude, calibrate altitude and gas resistance data will print on serial window.
 *
 * @copyright   Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author Martin(Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 * @url https://github.com/DFRobot/DFRobot_BME680
 */

#include "dfrobot_bme680_i2c.h"

#define I2C_TASK_PRIO                     24
#define I2C_TASK_STACK_SIZE               0x1000

float seaLevel;

void BME680_Task(void)
{

    DFRobot_BME680_I2C_INIT(CONFIG_I2C_SLAVE_ADDR, CONFIG_I2C_SCL_MASTER_PIN, CONFIG_I2C_SDA_MASTER_PIN, CONFIG_I2C_MASTER_BUS_ID);

    while(begin() != 0) {
        uapi_watchdog_kick();
        printf("bme begin failure\r\n");
        uapi_systick_delay_ms(2000);
    }
    printf("bme begin successful\r\n");
    static char templine[64] = {0};
#ifdef CONFIG_CALIBRATE_PRESSURE
    startConvert();
    uapi_systick_delay_ms(1000);
    update();
    /*You can use an accurate altitude to calibrate sea level air pressure. 
    *And then use this calibrated sea level pressure as a reference to obtain the calibrated altitude.
    *In this case,525.0m is chendu accurate altitude.
    */    
    seaLevel = readSeaLevel(525.0);
    sprintf(templine, "seaLevel: %.2f\r\n", seaLevel);
    printf(templine);
#endif

    while (1) {
        uapi_watchdog_kick();      
        startConvert();
        uapi_systick_delay_ms(1000);
        update();
        sprintf(templine, "temperature(C) : %.2f\r\n", readTemperature() / 100);
        printf(templine);
        sprintf(templine, "pressure(Pa) : %.2f\r\n", readPressure());
        printf(templine);
        sprintf(templine, "humidity(rh) : %.2f\r\n", readHumidity() / 1000);
        printf(templine);
        sprintf(templine, "gas resistance(ohm) : %.2f\r\n", readGasResistance());
        printf(templine);
        sprintf(templine, "altitude(m) : %.2f\r\n", readAltitude());
        printf(templine);
#ifdef CONFIG_CALIBRATE_PRESSURE
        sprintf(templine, "calibrated altitude(m) : %.2f\r\n", readCalibratedAltitude(seaLevel));
        printf(templine);
#endif
        
    }
}

void BME680_Entry(void)
{
    uint32_t ret;
    osal_task *taskid;
    // 创建任务调度
    osal_kthread_lock();
    // 创建任务1
    taskid = osal_kthread_create((osal_kthread_handler)BME680_Task, NULL, "BME680_Task", I2C_TASK_STACK_SIZE);
    ret = osal_kthread_set_priority(taskid, I2C_TASK_PRIO);
    if (ret != OSAL_SUCCESS) {
        printf("create task1 failed .\n");
    }
    osal_kthread_unlock();
}
app_run(BME680_Entry);