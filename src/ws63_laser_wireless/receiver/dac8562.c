/**
 * @file dac8562.c
 * @brief DAC8562 SPI driver for WS63.
 */
#include "dac8562.h"
#include "config.h"
#include "gpio.h"
#include "pinctrl.h"
#include "spi.h"
#include "tcxo.h"

#define DAC_POWER_SETTLE_MS 30
#define DAC_RESET_SETTLE_MS 5
#define DAC_REF_SETTLE_MS 20
#define DAC_CMD_SETTLE_US 10

void dac8562_write_channel(uint8_t cmd, uint16_t value)
{
    uint8_t buf[3] = {
        cmd,
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
    };

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = buf;
    xfer.tx_bytes = sizeof(buf);

    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_LOW);
    uapi_spi_master_write(DAC_SPI_BUS, &xfer, 1000);
    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_HIGH);
    (void)uapi_tcxo_delay_us(DAC_CMD_SETTLE_US);
}

static void dac8562_configure_device(void)
{
    dac8562_write_channel(DAC_CMD_RESET, 0x0001);
    (void)uapi_tcxo_delay_ms(DAC_RESET_SETTLE_MS);
    dac8562_write_channel(DAC_CMD_PWR_UP, 0x0003);
    dac8562_write_channel(DAC_CMD_INT_REF_EN, 0x0001);
    (void)uapi_tcxo_delay_ms(DAC_REF_SETTLE_MS);
    dac8562_write_channel(DAC_CMD_GAIN, DAC_GAIN_B2_A2);
    dac8562_write_channel(DAC_CMD_LDAC_DIS, 0x0003);
    dac8562_write_xy(0, 0);
    dac8562_write_xy(0, 0);
}

errcode_t dac8562_init(void)
{
    errcode_t ret;

    uapi_pin_set_mode(DAC_CS_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(DAC_CS_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_HIGH);
    (void)uapi_tcxo_delay_ms(DAC_POWER_SETTLE_MS);

    uapi_pin_set_mode(DAC_SPI_CLK_PIN, DAC_SPI_PIN_MODE);
    uapi_pin_set_mode(DAC_SPI_MOSI_PIN, DAC_SPI_PIN_MODE);

    spi_attr_t attr = {0};
    attr.is_slave = false;
    attr.slave_num = 1;
    attr.bus_clk = 32000000;
    attr.freq_mhz = 10;
    attr.clk_polarity = 0;
    attr.clk_phase = 1;
    attr.frame_format = 0;
    attr.frame_size = HAL_SPI_FRAME_SIZE_8;
    attr.spi_frame_format = 0;

    ret = uapi_spi_init(DAC_SPI_BUS, &attr, NULL);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    (void)uapi_tcxo_delay_ms(1);
    dac8562_configure_device();

    return ERRCODE_SUCC;
}

void dac8562_recover(void)
{
    dac8562_configure_device();
}

void dac8562_write_xy(uint16_t x_val, uint16_t y_val)
{
    dac8562_write_channel(DAC_CMD_SETA_UPDATEA, x_val);
    dac8562_write_channel(DAC_CMD_SETB_UPDATEB, y_val);
}
