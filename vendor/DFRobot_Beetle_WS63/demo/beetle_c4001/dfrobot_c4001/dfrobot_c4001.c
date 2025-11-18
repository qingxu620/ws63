/*!
 * @file dfrobot_c4001.c
 * @brief Define the basic structure of the DFRobot_C4001 class, the implementation of the basic methods
 * @copyright    Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license The MIT License (MIT)
 * @author [Martin](Martin@dfrobot.com)
 * @version V1.0
 * @date 2025-09-29
 * @url https://github.com/DFRobot/DFRobot_C4001
 */
#include "dfrobot_c4001.h"

static sPrivateData_t _buffer;

#define TIME_OUT 0x64    ///< time out
#define SENSOR_UART_ID 2 // 替换成你初始化时使用的 UART ID
#define DELAY_MS 1

#define THE_UART_TRANSFER_SIZE 200
static uint8_t g_app_uart_rx_buff[THE_UART_TRANSFER_SIZE] = {0};
static uart_buffer_config_t g_app_uart_buffer_config = {.rx_buffer = g_app_uart_rx_buff,
                                                        .rx_buffer_size = THE_UART_TRANSFER_SIZE};

static void writeReg(uint8_t reg, const uint8_t *data, uint8_t len)
{
    (void)reg;
    if (len > sizeof(g_app_uart_rx_buff)) {
        osal_printk("UART write error: len too large\r\n");
        return;
    }

    // 拷贝数据到全局 buffer
    memcpy(g_app_uart_rx_buff, data, len);

    // 调用底层写接口发送
    int ret = uapi_uart_write(SENSOR_UART_ID, g_app_uart_rx_buff, len, 0);
    if (ret < 0) {
        osal_printk("UART write error: %d\r\n", ret);
    }
}

static int16_t readReg(uint8_t reg, uint8_t *data, uint8_t len)
{
    (void)reg;

    if (len > sizeof(g_app_uart_rx_buff)) {
        osal_printk("UART read error: len too large\r\n");
        return -1;
    }

    uint16_t received = 0;
    uint32_t start = uapi_systick_get_ms();

    while (uapi_systick_get_ms() - start < TIME_OUT) {
        // 把数据直接读到 g_app_uart_rx_buff
        int ret = uapi_uart_read(SENSOR_UART_ID, g_app_uart_rx_buff + received, len - received, 0);
        if (ret > 0) {
            received += ret;
            if (received >= len) {
                break; // 收够数据
            }
        }
    }

    if (received > 0) {
        // 拷贝到用户缓冲
        memcpy(data, g_app_uart_rx_buff, received);
    }

    return received;
}

static sResponseData_t anaysisResponse(uint8_t *data, uint8_t len, uint8_t count)
{
    sResponseData_t responseData;
    uint8_t space[5] = {0};
    uint8_t i = 0;
    uint8_t j = 0;
    for (i = 0; i < len; i++) {
        if (data[i] == 'R' && data[i + 1] == 'e' && data[i + 2] == 's') {
            break;
        }
    }
    if (i == len || i == 0) {
        responseData.status = false;
    } else {
        responseData.status = true;
        for (j = 0; i < len; i++) {
            if (data[i] == ' ') {
                space[j++] = i + 1;
            }
        }
        if (j != 0) {
            responseData.response1 = atof((const char *)(data + space[0]));
            if (j >= 2) {
                responseData.response2 = atof((const char *)(data + space[1]));
            }
            if (count == 3) {
                responseData.response3 = atof((const char *)(data + space[2]));
            }
        } else {
            responseData.response1 = 0.0;
            responseData.response2 = 0.0;
        }
    }
    return responseData;
}

static sAllData_t anaysisData(uint8_t *data, uint8_t len)
{
    sAllData_t allData;
    uint8_t location = 0;
    memset(&allData, 0, sizeof(sAllData_t));
    for (uint8_t i = 0; i < len; i++) {
        if (data[i] == '$') {
            location = i;
            break;
        }
    }
    if (location == len) {
        return allData;
    }
    if (0 == strncmp((const char *)(data + location), "$DFHPD", strlen("$DFHPD"))) {
        allData.sta.workMode = EXITMODE;
        allData.sta.workStatus = 1;
        allData.sta.initStatus = 1;
        if (data[location + 7] == '1') {
            allData.exist = 1;
        } else {
            allData.exist = 0;
        }
    } else if (0 == strncmp((const char *)(data + location), "$DFDMD", strlen("$DFDMD"))) {
        allData.sta.workMode = SPEEDMODE;
        allData.sta.workStatus = 1;
        allData.sta.initStatus = 1;
        char *token;
        char *parts[10]; // Let's say there are at most 10 parts
        int index = 0;   // Used to track the number of parts stored
        token = strtok((char *)(data + location), ",");
        while (token != NULL) {
            parts[index] = token; // Stores partial Pointers in an array
            if (index++ > 8) {
                break;
            }
            token = strtok(NULL, ","); // Continue to extract the next section
        }
        allData.target.number = atoi(parts[1]);
        allData.target.range = atof(parts[3]) * 100;
        allData.target.speed = atof(parts[4]) * 100;
        allData.target.energy = atof(parts[5]);
    } else {
    }
    return allData;
}

static bool sensorStop(void)
{
    uint8_t len = 0;
    uint8_t temp[200] = {0};
    writeReg(0, (uint8_t *)STOP_SENSOR, strlen(STOP_SENSOR));
    uapi_systick_delay_ms(1000 * DELAY_MS);
    len = readReg(0, temp, 200);
    while (1) {
        if (len != 0) {
            if (strstr((const char *)temp, "sensorStop") != NULL) {
                return true;
            }
        }
        memset(temp, 0, 200);
        uapi_systick_delay_ms(400 * DELAY_MS);
        writeReg(0, (uint8_t *)STOP_SENSOR, strlen(STOP_SENSOR));
        len = readReg(0, temp, 200);
    }
}

static sResponseData_t wRCMD(char *cmd1, uint8_t count)
{
    uint8_t len = 0;
    uint8_t temp[200] = {0};
    sResponseData_t responseData;
    sensorStop();
    writeReg(0, (uint8_t *)cmd1, strlen(cmd1));
    uapi_systick_delay_ms(100 * DELAY_MS);
    len = readReg(0, temp, 200);
    responseData = anaysisResponse(temp, len, count);
    uapi_systick_delay_ms(100 * DELAY_MS);
    writeReg(0, (uint8_t *)START_SENSOR, strlen(START_SENSOR));
    uapi_systick_delay_ms(100 * DELAY_MS);
    return responseData;
}

static void writeCMD(char *cmd1, char *cmd2, uint8_t count)
{
    sensorStop();
    writeReg(0, (uint8_t *)cmd1, strlen(cmd1));
    uapi_systick_delay_ms(100 * DELAY_MS);
    if (count > 1) {
        uapi_systick_delay_ms(100 * DELAY_MS);
        writeReg(0, (uint8_t *)cmd2, strlen(cmd2));
        uapi_systick_delay_ms(100 * DELAY_MS);
    }
    writeReg(0, (uint8_t *)SAVE_CONFIG, strlen(SAVE_CONFIG));
    uapi_systick_delay_ms(100 * DELAY_MS);
    writeReg(0, (uint8_t *)START_SENSOR, strlen(START_SENSOR));
    uapi_systick_delay_ms(100 * DELAY_MS);
}

void DFRobot_C4001_INIT(uint32_t baud, uint8_t txpin, uint8_t rxpin, uint8_t uart_id)
{
    // 1. 配置引脚
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(rxpin, PIN_IE_1);
#endif
    uapi_pin_set_mode(txpin, PIN_MODE_2);
    uapi_pin_set_mode(rxpin, PIN_MODE_2);

    // 2. 配置 UART 属性
    uart_attr_t attr = {
        .baud_rate = baud, .data_bits = UART_DATA_BIT_8, .stop_bits = UART_STOP_BIT_1, .parity = UART_PARITY_NONE};

    uart_pin_config_t pin_config = {.tx_pin = txpin, .rx_pin = rxpin, .cts_pin = PIN_NONE, .rts_pin = PIN_NONE};

    uapi_uart_deinit(uart_id);
    uapi_uart_init(uart_id, &pin_config, &attr, NULL, &g_app_uart_buffer_config);
}

sSensorStatus_t getStatus(void)
{
    sSensorStatus_t data;
    uint8_t temp[100] = {0};
    uint8_t len = 0;

    sAllData_t allData;
    readReg(0, temp, 100);
    writeReg(0, (uint8_t *)START_SENSOR, strlen(START_SENSOR));
    while (len == 0) {
        uapi_systick_delay_ms(1000 * DELAY_MS);
        len = readReg(0, temp, 100);
        allData = anaysisData(temp, len);
    }
    data.workStatus = allData.sta.workStatus;
    data.workMode = allData.sta.workMode;
    data.initStatus = allData.sta.initStatus;

    return data;
}

bool motionDetection(void)
{
    static bool old = false;
    uint8_t status = 0;
    uint8_t len = 0;
    uint8_t temp[100] = {0};
    sAllData_t data;
    len = readReg(0, temp, 100);
    data = anaysisData(temp, len);
    if (data.exist) {
        old = (bool)status;
        return (bool)data.exist;
    } else {
        return (bool)old;
    }
}

void setSensor(eSetMode_t mode)
{
    if (mode == STARTSEN) {
        writeReg(0, (uint8_t *)START_SENSOR, strlen(START_SENSOR));
        uapi_systick_delay_ms(200 * DELAY_MS); // must timer
    } else if (mode == STOPSEN) {
        writeReg(0, (uint8_t *)STOP_SENSOR, strlen(STOP_SENSOR));
        uapi_systick_delay_ms(200 * DELAY_MS); // must timer
    } else if (mode == RESETSEN) {
        writeReg(0, (uint8_t *)RESET_SENSOR, strlen(RESET_SENSOR));
        uapi_systick_delay_ms(1500 * DELAY_MS); // must timer
    } else if (mode == SAVEPARAMS) {
        writeReg(0, (uint8_t *)STOP_SENSOR, strlen(STOP_SENSOR));
        uapi_systick_delay_ms(200 * DELAY_MS); // must timer
        writeReg(0, (uint8_t *)SAVE_CONFIG, strlen(SAVE_CONFIG));
        uapi_systick_delay_ms(800 * DELAY_MS); // must timer
        writeReg(0, (uint8_t *)START_SENSOR, strlen(START_SENSOR));
    } else if (mode == RECOVERSEN) {
        writeReg(0, (uint8_t *)STOP_SENSOR, strlen(STOP_SENSOR));
        uapi_systick_delay_ms(200 * DELAY_MS);
        writeReg(0, (uint8_t *)RECOVER_SENSOR, strlen(RECOVER_SENSOR));
        uapi_systick_delay_ms(800 * DELAY_MS); // must timer
        writeReg(0, (uint8_t *)START_SENSOR, strlen(START_SENSOR));
        uapi_systick_delay_ms(500 * DELAY_MS);
    }
}

bool setSensorMode(eMode_t mode)
{
    sensorStop();
    if (mode == EXITMODE) {
        writeReg(0, (uint8_t *)EXIST_MODE, strlen(EXIST_MODE));
        uapi_systick_delay_ms(50 * DELAY_MS);
    } else {
        writeReg(0, (uint8_t *)SPEED_MODE, strlen(SPEED_MODE));
        uapi_systick_delay_ms(50 * DELAY_MS);
    }
    uapi_systick_delay_ms(50 * DELAY_MS);
    writeReg(0, (uint8_t *)SAVE_CONFIG, strlen(SAVE_CONFIG));
    uapi_systick_delay_ms(500 * DELAY_MS);
    writeReg(0, (uint8_t *)START_SENSOR, strlen(START_SENSOR));
    uapi_systick_delay_ms(100 * DELAY_MS);
    return true;
}

bool setTrigSensitivity(uint8_t sensitivity)
{
    if (sensitivity > 9) {
        return false;
    }

    char data[] = "setSensitivity 255 1"; // 分配在栈上，可写
    data[19] = sensitivity + '0';         // 修改有效
    writeCMD(data, data, (uint8_t)1);
    return true;
}

uint8_t getTrigSensitivity(void)
{
    sResponseData_t responseData;
    uint8_t temp[100] = {0};
    readReg(0, temp, 100);
    char *data = "getSensitivity";
    responseData = wRCMD(data, (uint8_t)1);
    if (responseData.status) {
        return responseData.response1;
    }
    return 0;
}

bool setKeepSensitivity(uint8_t sensitivity)
{
    if (sensitivity > 9) {
        return false;
    }

    char data[] = "setSensitivity 1 255";
    data[15] = sensitivity + '0';
    writeCMD(data, data, (uint8_t)1);
    return true;
}

uint8_t getKeepSensitivity(void)
{
    sResponseData_t responseData;
    uint8_t temp[100] = {0};
    readReg(0, temp, 100);
    char *data = "getSensitivity";
    responseData = wRCMD(data, (uint8_t)1);
    if (responseData.status) {
        return responseData.response2;
    }
    return 0;
}

bool setDelay(uint8_t trig, uint16_t keep)
{
    if (trig > 200) {
        return false;
    }
    if (keep < 4 || keep > 3000) {
        return false;
    }

    // trig: 百分比 -> 秒 (trig * 0.01)
    float trig_val = trig * 0.01f;
    // keep: 乘0.5
    float keep_val = keep * 0.5f;

    char cmd[64] = {0};
    // 格式化命令字符串
    snprintf(cmd, sizeof(cmd), "setLatency %.1f %.1f", trig_val, keep_val);

    // 调用底层写命令函数
    writeCMD(cmd, cmd, 1);

    return true;
}

uint8_t getTrigDelay(void)
{
    sResponseData_t responseData;
    char *data = "getLatency";
    responseData = wRCMD(data, (uint8_t)1);
    if (responseData.status) {
        return responseData.response1 * 100;
    }
    return 0;
}

uint16_t getKeepTimerout(void)
{
    sResponseData_t responseData;
    char *data = "getLatency";
    responseData = wRCMD(data, (uint8_t)2);
    if (responseData.status) {
        return responseData.response2 * 2;
    }
    return 0;
}

bool setDetectionRange(uint16_t min, uint16_t max, uint16_t trig)
{
    if (max < 240 || max > 2000) {
        return false;
    }
    if (min < 30 || min > max) {
        return false;
    }

    float min_val = min / 100.0f;
    float max_val = max / 100.0f;
    float trig_val = trig / 100.0f;

    char data1[64] = {0};
    char data2[64] = {0};

    // 拼接命令
    snprintf(data1, sizeof(data1), "setRange %.2f %.2f", min_val, max_val);
    snprintf(data2, sizeof(data2), "setTrigRange %.2f", trig_val);

    // 发送命令
    writeCMD(data1, data2, 2);

    return true;
}

uint16_t getTrigRange(void)
{
    sResponseData_t responseData;
    char *data = "getTrigRange";
    responseData = wRCMD(data, (uint8_t)1);
    if (responseData.status) {
        return responseData.response1 * 100;
    }
    return 0;
}

uint16_t getMaxRange(void)
{
    sResponseData_t responseData;
    char *data = "getRange";
    responseData = wRCMD(data, (uint8_t)2);
    if (responseData.status) {
        return responseData.response2 * 100;
    }
    return 0;
}

uint16_t getMinRange(void)
{
    sResponseData_t responseData;
    char *data = "getRange";
    responseData = wRCMD(data, (uint8_t)2);
    if (responseData.status) {
        return responseData.response1 * 100;
    }
    return 0;
}

uint8_t getTargetNumber(void)
{
    static uint8_t flash_number = 0;
    uint8_t len = 0;
    uint8_t temp[100] = {0};
    sAllData_t data;
    len = readReg(0, temp, 100);
    data = anaysisData(temp, len);
    if (data.target.number != 0) {
        flash_number = 0;
        _buffer.number = data.target.number;
        _buffer.range = data.target.range / 100.0;
        _buffer.speed = data.target.speed / 100.0;
        _buffer.energy = data.target.energy;
    } else {
        _buffer.number = 1;
        if (flash_number++ > 10) {
            _buffer.number = 0;
            _buffer.range = 0;
            _buffer.speed = 0;
            _buffer.energy = 0;
        }
    }
    return data.target.number;
}

float getTargetSpeed(void)
{
    return _buffer.speed;
}

float getTargetRange(void)
{
    return _buffer.range;
}

uint32_t getTargetEnergy(void)
{
    return _buffer.energy;
}

bool setDetectThres(uint16_t min, uint16_t max, uint16_t thres)
{
    if (max > 2500) {
        return false;
    }
    if (min > max) {
        return false;
    }

    float min_val = min / 100.0f;
    float max_val = max / 100.0f;

    char data1[64] = {0};
    char data2[64] = {0};

    // 拼接字符串
    snprintf(data1, sizeof(data1), "setRange %.2f %.2f", min_val, max_val);
    snprintf(data2, sizeof(data2), "setThrFactor %u", thres);

    // 发送命令
    writeCMD(data1, data2, 2);

    return true;
}

bool setIoPolaity(uint8_t value)
{
    if (value > 1) {
        return false;
    }

    char data[32] = {0};                                   // 可写缓冲
    snprintf(data, sizeof(data), "setGpioMode %d", value); // 拼接字符串
    writeCMD(data, data, (uint8_t)1);
    return true;
}

uint8_t getIoPolaity(void)
{
    sResponseData_t responseData;
    char *data = "getGpioMode 1";
    responseData = wRCMD(data, (uint8_t)2);
    if (responseData.status) {
        return responseData.response2;
    }
    return 0;
}

bool setPwm(uint8_t pwm1, uint8_t pwm2, uint8_t timer)
{
    if (pwm1 > 100 || pwm2 > 100) {
        return false;
    }

    char data[64] = {0};

    // 拼接成命令字符串，例如：setPwm 50 75 10
    snprintf(data, sizeof(data), "setPwm %u %u %u", pwm1, pwm2, timer);

    // 发送命令
    writeCMD(data, data, 1);

    return true;
}

sPwmData_t getPwm(void)
{
    sPwmData_t pwmData;
    memset(&pwmData, 0, sizeof(sPwmData_t));

    sResponseData_t responseData;
    char *data = "getPwm";
    responseData = wRCMD(data, (uint8_t)3);
    if (responseData.status) {
        pwmData.pwm1 = responseData.response1;
        pwmData.pwm2 = responseData.response2;
        pwmData.timer = responseData.response3;
    }
    return pwmData;
}

uint16_t getTMinRange(void)
{
    sResponseData_t responseData;
    char *data = "getRange";
    responseData = wRCMD(data, (uint8_t)1);
    if (responseData.status) {
        return responseData.response1 * 100;
    }
    return 0;
}

uint16_t getTMaxRange(void)
{
    sResponseData_t responseData;
    char *data = "getRange";
    responseData = wRCMD(data, (uint8_t)2);
    if (responseData.status) {
        return responseData.response2 * 100;
    }
    return 0;
}

uint16_t getThresRange(void)
{
    sResponseData_t responseData;
    char *data = "getThrFactor";
    responseData = wRCMD(data, (uint8_t)1);
    if (responseData.status) {
        return responseData.response1;
    }
    return 0;
}

void setFrettingDetection(eSwitch_t sta)
{
    char data[32] = {0};
    snprintf(data, sizeof(data), "setMicroMotion %d", sta);
    writeCMD(data, data, (uint8_t)1);
}

eSwitch_t getFrettingDetection(void)
{
    sResponseData_t responseData;
    char *data = "getMicroMotion";
    responseData = wRCMD(data, (uint8_t)1);
    if (responseData.status) {
        return (eSwitch_t)responseData.response1;
    }
    return (eSwitch_t)0;
}
