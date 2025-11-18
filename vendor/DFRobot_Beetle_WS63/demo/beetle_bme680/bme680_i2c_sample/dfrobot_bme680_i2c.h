/**
 * @file dfrobot_bme680_i2c.h
 *
 * @copyright   Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author Martin(Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 * @url https://github.com/DFRobot/DFRobot_BME680
 */

#ifndef DFRobot_BME680_I2C_H
#define DFRobot_BME680_I2C_H

#include "dfrobot_bme680.h"
#include "i2c.h"

void DFRobot_BME680_I2C_INIT(uint8_t I2CAddr,
                             uint8_t iic_scl_master_pin,
                             uint8_t iic_sda_master_pin,
                             uint8_t iic_bus_id);

#endif