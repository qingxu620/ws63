/**
 * @file dfrobot_bme680.h
 *
 * @copyright   Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author Martin(Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 * @url https://github.com/DFRobot/DFRobot_BME680
 */

#ifndef DFROBOT_BME680_H
#define DFROBOT_BME680_H

#include "bme680.h"

#include "pinctrl.h"
#include "common_def.h"
#include "soc_osal.h"
#include "gpio.h"
#include "systick.h"
#include "osal_debug.h"
#include "watchdog.h"
#include "app_init.h"



#define BME680_SEALEVEL 1015

/**\name C standard macros */
#ifndef NULL
#ifdef __cplusplus
#define NULL   0
#else
#define NULL   ((void *) 0)
#endif
#endif

typedef enum {
  eBME680_INTERFACE_SPI,
  eBME680_INTERFACE_I2C
} eBME680_INTERFACE;

typedef void (*pfStartConvert_t)(void);
typedef void (*pfUpdate_t)(void);

void bme680_delay_ms(uint32_t period);

typedef enum {
  eBME680_PARAM_TEMPSAMP,
  eBME680_PARAM_HUMISAMP,
  eBME680_PARAM_PREESAMP,
  eBME680_PARAM_IIRSIZE
} eBME680_param_t;


void DFRobot_BME680(bme680_com_fptr_t readReg, bme680_com_fptr_t writeReg, bme680_delay_fptr_t delayMS, eBME680_INTERFACE interface);

extern uint8_t bme680_I2CAddr;
/**
 * @fn begin
 * @brief begin BME680 device
 * @return result
 * @retval  non-zero : failed
 * @retval  0        : succussful
 */
int16_t begin(void);
/**
 * @fn update
 * @brief update all data to MCU ram
 */
void    update(void);

/**
 * @fn startConvert
 * @brief start convert to get a accurate values
 */
void  startConvert(void);
/**
 * @fn readTemperature
 * @brief read the temperature value (unit C)
 *
 * @return temperature valu, this value has two decimal points
 */
float readTemperature(void);
/**
 * @fn readPressure
 * @brief read the pressure value (unit pa)
 *
 * @return pressure value, this value has two decimal points
 */
float readPressure(void);
/**
 * @fn readHumidity
 * @brief read the humidity value (unit %rh)
 * @return humidity value, this value has two decimal points
 */
float readHumidity(void);
/**
 * @fn readAltitude
 * @brief read the altitude (unit meter)
 * @return altitude value, this value has two decimal points
 */
float readAltitude(void);
/**
 * @fn readCalibratedAltitude
 * @brief read the Calibrated altitude (unit meter)
 *
 * @param seaLevel  normalised atmospheric pressure
 *
 * @return calibrated altitude value , this value has two decimal points
 */
float readCalibratedAltitude(float seaLevel);
/**
 * @fn readGasResistance
 * @brief read the gas resistance(unit ohm)
 * @return temperature value, this value has two decimal points
 */
float readGasResistance(void);
/**
 * @fn readSeaLevel
 * @brief read normalised atmospheric pressure (unit pa)
 * @param altitude   accurate altitude for normalising
 * @return normalised atmospheric pressure
 */
float readSeaLevel(float altitude);
/**
 * @fn setParam
 * @brief set bme680 parament
 *
 * @param eParam        :which param you want to change
 *        dat           :object data, can't more than 5
 */  
void setParam(eBME680_param_t eParam, uint8_t dat);
/**
 * @fn setGasHeater
 * @brief set bme680 gas heater
 * @param temp        :your object temp
 * @param t           :time spend in milliseconds
 */
void setGasHeater(uint16_t temp, uint16_t t);


void writeParamHelper(uint8_t reg, uint8_t dat, uint8_t addr);

#endif


