/*!
 * @file dfrobot_lis2dh12.c
 * @brief Define the basic structure of DFRobot_LIS2DH12 
 * @copyright   Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author [Martin](Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 * @url https://github.com/DFRobot/DFRobot_LIS
 */

#include "dfrobot_lis2dh12.h"

#define I2C_MASTER_ADDR                   0x0
#define I2C_SET_BAUDRATE                  400000
#define I2C_MASTER_PIN_MODE               2

uint8_t _iic_bus_id;
uint8_t _deviceAddr;
uint8_t _mgScaleVel = 16;
uint8_t _reset = 0;

bool DFRobot_LIS2DH12_INIT(uint8_t addr, uint8_t iic_scl_master_pin, uint8_t iic_sda_master_pin, uint8_t iic_bus_id)
{
    _deviceAddr = addr;
    _iic_bus_id = iic_bus_id;
    uapi_pin_set_mode(iic_scl_master_pin, I2C_MASTER_PIN_MODE);
    uapi_pin_set_mode(iic_sda_master_pin, I2C_MASTER_PIN_MODE);
    uapi_i2c_master_init(_iic_bus_id, I2C_SET_BAUDRATE, I2C_MASTER_ADDR);

    uint8_t identifier = 0;
    bool ret = false;
    _reset = 1;
    readReg(REG_CARD_ID,&identifier,1);
    DBG(identifier);
    if(identifier == 0x33){
      ret = true;
    }else if(identifier == 0 || identifier == 0xff){
      DBG("Communication failure");
      ret = false;
    }else{
      DBG("the ic is not LIS2DH12");
      ret = false;
    }
    return ret;
}

void writeReg(uint8_t reg, const void * pBuf, size_t size)
{
    if (pBuf == NULL) {
        osal_printk("pBuf ERROR!! : null pointer\n");
        return;
    }

    // 拼接寄存器地址 + 数据
    uint8_t buf[1 + size];
    buf[0] = reg;
    memcpy(&buf[1], pBuf, size);

    i2c_data_t data = {0};
    data.send_buf   = buf;
    data.send_len   = 1 + size;
    data.receive_buf = NULL;
    data.receive_len = 0;

    if (uapi_i2c_master_write(_iic_bus_id, _deviceAddr, &data) != ERRCODE_SUCC) {
        osal_printk("I2C writeReg error!\n");
    }
}

uint8_t readReg(uint8_t reg, void* pBuf, size_t size)
{
    if (pBuf == NULL) {
        osal_printk("pBuf ERROR!! : null pointer\n");
        return 0;
    }

    i2c_data_t data = {0};
    data.send_buf    = &reg;
    data.send_len    = 1;       // 先发寄存器地址
    data.receive_buf = pBuf;
    data.receive_len = size;    // 再收数据

    if (uapi_i2c_master_write(_iic_bus_id, _deviceAddr, &data) != ERRCODE_SUCC) {
        osal_printk("I2C readReg send error!\n");
        return 0;
    }

    if (uapi_i2c_master_read(_iic_bus_id, _deviceAddr, &data) != ERRCODE_SUCC) {
        osal_printk("I2C readReg recv error!\n");
        return 0;
    }

    return size;
}


/**
 * @fn limitAccelerationData
 * @brief Limit acceleration data based on measurement range
 * @param data Acceleration data to be limited
 * @return Limited acceleration data
 */
static int32_t limitAccelerationData(int32_t data)
{
  // Set appropriate limit values based on measurement range
  if(_mgScaleVel == 16){      // 2g range
    if(data > 2000)
      data = 2000;
    else if(data < -2000)
      data = -2000;
  }
  else if(_mgScaleVel == 32){ // 4g range
    if(data > 4000)
      data = 4000;
    else if(data < -4000)
      data = -4000;
  }
  else if(_mgScaleVel == 64){ // 8g range
    if(data > 8000)
      data = 8000;
    else if(data < -8000)
      data = -8000;
  }
  else if(_mgScaleVel == 192){ // 16g range
    if(data > 16000)
      data = 16000;
    else if(data < -16000)
      data = -16000;
  }
  return data;
}

int32_t readAccX()
{
  int8_t sensorData[2];
  int32_t data;
  readReg(REG_OUT_X_L|0x80,sensorData,2);
  data = (sensorData[1]  * (uint8_t)_mgScaleVel);

  return limitAccelerationData(data);
}

int32_t readAccY()
{
  int8_t sensorData[2];
  int32_t a;
  readReg(REG_OUT_Y_L|0x80, sensorData, 2);
  a = (sensorData[1]  * (uint8_t)_mgScaleVel);

  return limitAccelerationData(a);
}

int32_t readAccZ()
{
  int8_t sensorData[2];
  int32_t a;
  readReg(REG_OUT_Z_L|0x80, sensorData, 2);
  DBG(sensorData[0]);
  DBG(sensorData[1]);
  a = (sensorData[1]* (uint8_t)_mgScaleVel);
  return limitAccelerationData(a);
}

void setRange(eRange_t range)
{
  switch(range){
    case eLIS2DH12_2g:
      _mgScaleVel = 16;
      break;
    case eLIS2DH12_4g:
      _mgScaleVel = 32;
      break;
    case eLIS2DH12_8g:
      _mgScaleVel = 64;
      break;
    default:
      _mgScaleVel = 192;
      break;
  }
  DBG(range);
  writeReg(REG_CTRL_REG4,&range,1);
}


void setAcquireRate(ePowerMode_t rate)
{
  uint8_t reg = 0x0f;
  reg = reg | rate;
  DBG(reg);
  writeReg(REG_CTRL_REG1,&reg,1);
}

uint8_t getID()
{
  uint8_t identifier; 
  readReg(REG_CARD_ID,&identifier,1);
  return identifier;
}

void setInt1Th(uint8_t threshold)
{
    uint8_t reg = (threshold * 1024)/_mgScaleVel;
    uint8_t reg1 = 0x08;
    uint8_t reg2 = 0x00;
    uint8_t data = 0x40;

    writeReg(REG_CTRL_REG2,&reg2,1);
    writeReg(REG_CTRL_REG3,&data,1);
    writeReg(REG_CTRL_REG5,&reg1,1);
    writeReg(REG_CTRL_REG6,&reg2,1);
    writeReg(REG_INT1_THS,&reg,1);
    readReg(REG_CTRL_REG5,&reg2,1);
    DBG(reg2);
    readReg(REG_CTRL_REG3,&reg2,1);
    DBG(reg2);
}

void setInt2Th(uint8_t threshold)
{
    uint8_t reg = (threshold * 1024)/_mgScaleVel;
    uint8_t reg1 = 0x02;
    uint8_t reg2 = 0x00;
    uint8_t data = 0x40;

    writeReg(REG_CTRL_REG2,&reg2,1);
    writeReg(REG_CTRL_REG3,&reg2,1);
    writeReg(REG_CTRL_REG5,&reg1,1);
    writeReg(REG_CTRL_REG6,&data,1);
    writeReg(REG_INT2_THS,&reg,1);
    readReg(REG_CTRL_REG5,&reg2,1);
    DBG(reg2);
    readReg(REG_CTRL_REG6,&reg2,1);
    DBG(reg2);
}

void enableInterruptEvent(eInterruptSource_t source,eInterruptEvent_t event)
{
   uint8_t data = 0;
  data = 0x80 | event;
  DBG(data);
  if(source == eINT1)
    writeReg(REG_INT1_CFG,&data,1);
  else
    writeReg(REG_INT2_CFG,&data,1);

  readReg(REG_INT1_CFG,&data,1);
  DBG(data);
}

bool getInt1Event(eInterruptEvent_t event)
{
  uint8_t data = 0;
  bool ret = false;
  readReg(REG_INT1_SRC,&data,1);
  DBG(data,HEX);
  if(data & 0x40){
    switch(event){
      case eXLowerThanTh:
        if(!(data & 0x01))
          ret = true;
        break;
      case eXHigherThanTh:
        if((data & 0x02) == 0x02)
          ret = true;
        break;
      case eYLowerThanTh:
        if(!(data & 0x04))
          ret = true;
        break;
      case eYHigherThanTh:
        if((data & 0x08) == 0x08)
          ret = true;
        break;
      case eZLowerThanTh:
        if(!(data & 0x10))
          ret = true;
        break;
      case eZHigherThanTh:
        if((data & 0x20) == 0x20)
          ret = true;
        break;
      default:
        ret = false;
    }
  }else{
    ret = false;
  }
  return ret;
}

bool getInt2Event(eInterruptEvent_t event)
{
  uint8_t data = 0;
  bool ret = false;
  readReg(REG_INT2_SRC,&data,1);
  DBG(data,HEX);
  if(data & 0x40){
    switch(event){
      case eXLowerThanTh:
        if(!(data & 0x01))
          ret = true;
        break;
      case eXHigherThanTh:
        if((data & 0x02) == 0x02)
          ret = true;
        break;
      case eYLowerThanTh:
        if(!(data & 0x04))
          ret = true;
        break;
      case eYHigherThanTh:
        if((data & 0x08) == 0x08)
          ret = true;
        break;
      case eZLowerThanTh:
        if(!(data & 0x10))
          ret = true;
        break;
      case eZHigherThanTh:
        if((data & 0x20) == 0x20)
          ret = true;
        break;
      default:
        ret = false;
    }
  }else{
    ret = false;
  }
  return ret;
}