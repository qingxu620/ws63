/**
 * @file dac8562.c
 * @brief DAC8562 SPI 驱动实现 (WS63 移植版)
 *        重写自 Arduino DAC8562 库，消除双重字节序反转
 *        SPI 协议: 24-bit (8-bit cmd + 16-bit data), MSB first, Mode 1
 */
#include "dac8562.h"
#include "config.h"
#include "pinctrl.h"
#include "gpio.h"
#include "spi.h"
#include "soc_osal.h"

/**
 * @brief 向 DAC8562 发送 3 字节 SPI 命令
 *        字节序: [cmd] [data_high] [data_low]  MSB first
 *        这里直接按正确顺序发送，没有 Arduino 原始库的双重反转问题
 */
void dac8562_write_channel(uint8_t cmd, uint16_t value)
{
    uint8_t buf[3] = {
        cmd, (uint8_t)((value >> 8) & 0xFF), /* 数据高字节 */
        (uint8_t)(value & 0xFF)              /* 数据低字节 */
    };

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = buf;
    xfer.tx_bytes = sizeof(buf);

    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_LOW);
    uapi_spi_master_write(DAC_SPI_BUS, &xfer, 1000);
    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_HIGH);
}

errcode_t dac8562_init(void)
{
    errcode_t ret;

    /* 1. 配置 SPI 引脚复用 */
    uapi_pin_set_mode(DAC_SPI_CLK_PIN, DAC_SPI_PIN_MODE);  /* GPIO7 → SPI0_SCK  */
    uapi_pin_set_mode(DAC_SPI_MOSI_PIN, DAC_SPI_PIN_MODE); /* GPIO9 → SPI0_OUT  */

    /* 2. SPI 初始化 — Mode 1 (CPOL=0, CPHA=1), MSB first */
    spi_attr_t attr = {0};
    attr.is_slave = false;
    attr.slave_num = 1;
    attr.bus_clk = 32000000;                /* 32MHz 系统时钟 */
    attr.freq_mhz = 10;                     /* SPI 时钟 10MHz */
    attr.clk_polarity = 0;                  /* CPOL = 0 */
    attr.clk_phase = 1;                     /* CPHA = 1 —— SPI Mode 1 */
    attr.frame_format = 0;                  /* Motorola */
    attr.frame_size = HAL_SPI_FRAME_SIZE_8; /* 8-bit frame */
    attr.spi_frame_format = 0;

    ret = uapi_spi_init(DAC_SPI_BUS, &attr, NULL);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    /* 3. CS 引脚配置为 GPIO 输出，手动控制 */
    uapi_pin_set_mode(DAC_CS_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(DAC_CS_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_HIGH); /* 默认不选中 */

    /* 3. DAC 初始化序列 */
    dac8562_write_channel(DAC_CMD_RESET, 0x0001); /* 软复位 */
    osal_udelay(100);
    dac8562_write_channel(DAC_CMD_PWR_UP, 0x0003);       /* 上电 DAC-A + DAC-B */
    dac8562_write_channel(DAC_CMD_INT_REF_EN, 0x0001);   /* 使能内部参考电压 */
    dac8562_write_channel(DAC_CMD_GAIN, DAC_GAIN_B2_A2); /* 增益 2x */
    dac8562_write_channel(DAC_CMD_LDAC_DIS, 0x0003);     /* LDAC 直通模式 */

    /* 4. DAC 输出归零 */
    dac8562_write_channel(DAC_CMD_SETA_UPDATEA, 0);
    dac8562_write_channel(DAC_CMD_SETB_UPDATEB, 0);

    return ERRCODE_SUCC;
}

void dac8562_write_xy(uint16_t x_val, uint16_t y_val)
{
    dac8562_write_channel(DAC_CMD_SETA_UPDATEA, x_val);
    dac8562_write_channel(DAC_CMD_SETB_UPDATEB, y_val);
}
