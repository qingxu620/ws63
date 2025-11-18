/*!
 * @file bme680_spi_sample.c
 * @brief Connect bme680 4 wires SPI interface with your board (please reference board compatibility).
 * @n Temprature, Humidity, pressure, altitude, calibrate altitude and gas resistance data will print on serial window.
 *
 * @copyright   Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author Martin(Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 * @url https://github.com/DFRobot/DFRobot_BME680
 */

#include "dfrobot_bme680_spi.h"

#define SPI_TASK_PRIO 24
#define SPI_TASK_STACK_SIZE 0x1000
#define DELAY_S 1000
#define TEMP_RAW_TO_CELSIUS 100
#define HUMIDITY_RAW_TO_PERCENT 1000

float seaLevel;

void BME680_Task(void)
{

    DFRobot_BME680_SPI_INIT(CONFIG_SPI_CS_MASTER_PIN, CONFIG_SPI_DI_MASTER_PIN, CONFIG_SPI_DO_MASTER_PIN,
                            CONFIG_SPI_CLK_MASTER_PIN, CONFIG_SPI_MASTER_BUS_ID);

    int16_t ret = begin();
    while (ret != 0) {
        uapi_watchdog_kick();
        printf("bme begin failure ret = %d\r\n", ret);
        uapi_systick_delay_ms(DELAY_S * 2);
        ret = begin();
    }
    printf("bme begin successful\r\n");
    static char templine[64] = {0};
#ifdef CONFIG_CALIBRATE_PRESSURE
    startConvert();
    uapi_systick_delay_ms(DELAY_S);
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
        uapi_systick_delay_ms(DELAY_S);
        update();
        sprintf(templine, "temperature(C) : %.2f\r\n", readTemperature() / TEMP_RAW_TO_CELSIUS);
        printf(templine);
        sprintf(templine, "pressure(Pa) : %.2f\r\n", readPressure());
        printf(templine);
        sprintf(templine, "humidity(rh) : %.2f\r\n", readHumidity() / HUMIDITY_RAW_TO_PERCENT);
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
    taskid = osal_kthread_create((osal_kthread_handler)BME680_Task, NULL, "BME680_Task", SPI_TASK_STACK_SIZE);
    ret = osal_kthread_set_priority(taskid, SPI_TASK_PRIO);
    if (ret != OSAL_SUCCESS) {
        printf("create task1 failed .\n");
    }
    osal_kthread_unlock();
}
app_run(BME680_Entry);