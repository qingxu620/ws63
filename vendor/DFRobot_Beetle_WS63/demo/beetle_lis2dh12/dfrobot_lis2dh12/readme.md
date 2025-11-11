# dfrobot_lis2dh12

LIS2DH12是一款超低功率高性能三轴线性
加速度计属于“femto”家族，利用了强大的和
成熟的制造工艺已用于生产微加工
加速度计。
LIS2DH12有用户可选择的±2g/±4g/±8g/±16g的全刻度
![产品效果图片](./resources/images/SEN0224_TOP.jpg)

## 产品链接(https://www.dfrobot.com.cn/goods-1372.html)
    SKU:SEN0224

## 目录

  * [概述](#概述)
  * [API](#API)
  * [历史](#历史)

## 概述

提供一个WS63库，通过读取LIS2DH12数据获得三轴加速度。

## API
```C++
  /**
   * @fn DFRobot_LIS2DH12_INIT
   * @brief 初始化函数
   * @param addr 从机I2C地址
   * @param iic_scl_master_pin SCL引脚
   * @param iic_sda_master_pin SDA引脚
   * @param iic_bus_id I2C总线标识符
   * @return true(成功)/false(失败)
   */
  bool DFRobot_LIS2DH12_INIT(uint8_t addr, uint8_t iic_scl_master_pin, uint8_t iic_sda_master_pin, uint8_t iic_bus_id);

  /**
   * @fn setRange
   * @brief 设置测量范围
   * @param range 范围(g)
   * @n           eLIS2DH12_2g, //±2g
   * @n           eLIS2DH12_4g, //4g
   * @n           eLIS2DH12_8g, //8g
   * @n           eLIS2DH12_16g, //16g
   */
  void setRange(eRange_t range);

  /**
   * @fn setAcquireRate
   * @brief 设置数据测量速率
   * @param rate 速度(HZ)
   * @n          ePowerDown_0Hz 
   * @n          eLowPower_1Hz 
   * @n          eLowPower_10Hz 
   * @n          eLowPower_25Hz 
   * @n          eLowPower_50Hz 
   * @n          eLowPower_100Hz
   * @n          eLowPower_200Hz
   * @n          eLowPower_400Hz
   */
  void setAcquireRate(ePowerMode_t rate);

  /**
   * @fn setAcquireRate
   * @brief 获取芯片ID
   * @return 8连续数据
   */
  uint8_t getID();

  /**
   * @fn readAccX
   * @brief 获取x方向上的加速度
   * @return 加速度为x(单位:g)，测量范围为±100g或±200g，由setRange()函数设定。
   */
  int32_t readAccX();

  /**
   * @fn readAccY
   * @brief 获取y方向的加速度
   * @return 加速度为y(单位:g)，测量范围为±100g或±200g，由setRange()函数设定。
   */
  int32_t readAccY();

  /**
   * @fn readAccZ
   * @brief 获取z方向的加速度
   * @return 加速度从z开始(单位:g)，测量范围为±100g或±200g，由setRange()函数设定。
   */
  int32_t readAccZ();
  
  /**
   * @fn setInt1Th
   * @brief 设置中断源1中断的阈值
   * @param threshold 告警阈值在测量范围内，单位:g
   */
  void setInt1Th(uint8_t threshold);

  /**
   * @fn setInt2Th
   * @brief 设置“中断源2”的中断产生阈值
   * @param threshold 告警阈值在测量范围内，单位:g
   */
  void setInt2Th(uint8_t threshold);

  /**
   * @fn enableInterruptEvent
   * @brief 启用中断
   * @param source 中断引脚选择
   * @n           eINT1 = 0,/<int1 >/
   * @n           eINT2,/<int2>/
   * @param event 中断事件选择
   * @n            eXLowerThanTh ,/<x方向上的加速度小于阈值>/
   * @n            eXHigherThanTh ,/<x方向上的加速度大于阈值>/
   * @n            eYLowerThanTh,/<y方向上的加速度小于阈值>/
   * @n            eYHigherThanTh,/<y方向上的加速度大于阈值>/
   * @n            eZLowerThanTh,/<z方向的加速度小于阈值>/
   * @n            eZHigherThanTh,/<z方向的加速度大于阈值>/
   */
  void enableInterruptEvent(eInterruptSource_t source, eInterruptEvent_t event);

  /**
   * @fn getInt1Event
   * @brief 检查中断1中是否产生中断事件'event'  
   * @param event Interrupt event
   * @n            eXLowerThanTh ,/<x方向上的加速度小于阈值>/
   * @n            eXHigherThanTh ,/<x方向上的加速度大于阈值>/
   * @n            eYLowerThanTh,/<y方向上的加速度小于阈值>/
   * @n            eYHigherThanTh,/<y方向上的加速度大于阈值>/
   * @n            eZLowerThanTh,/<z方向的加速度小于阈值>/
   * @n            eZHigherThanTh,/<z方向的加速度大于阈值>/
   * @return true 产生/false 没有产生
   */
  bool getInt1Event(eInterruptEvent_t event);

  /**
   * @fn getInt2Event
   * @brief 检查中断2中是否产生中断事件'event'  
   * @param event Interrupt event
   * @n            eXLowerThanTh ,/<x方向上的加速度小于阈值>/
   * @n            eXHigherThanTh ,/<x方向上的加速度大于阈值>/
   * @n            eYLowerThanTh,/<y方向上的加速度小于阈值>/
   * @n            eYHigherThanTh,/<y方向上的加速度大于阈值>/
   * @n            eZLowerThanTh,/<z方向的加速度小于阈值>/
   * @n            eZHigherThanTh,/<z方向的加速度大于阈值>/
   * @return true 产生/false 没有产生
   */
  bool getInt2Event(eInterruptEvent_t event);
```
## 历史

- 2025/9/29 - WS63版本 V1.0.0 - Written by(Martin@dfrobot.com), 2025.
- 2021/2/1 - Arduino版本 V1.0.0 - Written by(li.feng@dfrobot.com,jie.tang@dfrobot.com), 2021.
