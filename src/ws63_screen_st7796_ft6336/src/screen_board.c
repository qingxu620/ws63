/**
 * @file screen_board.c
 * @brief WS63 hardware adapter for the ST7796 + FT6336 screen board.
 */
#include "screen_board.h"
#include "screen_config.h"

#include "gpio.h"
#include "i2c.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "spi.h"
#include "tcxo.h"

static void screen_gpio_output(pin_t pin, gpio_level_t level)
{
    uapi_pin_set_mode(pin, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(pin, level);
}

static void screen_gpio_input(pin_t pin)
{
    uapi_pin_set_mode(pin, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
}

errcode_t screen_board_init(void)
{
    screen_gpio_output(SCREEN_LCD_CS_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_output(SCREEN_LCD_DC_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_output(SCREEN_LCD_RST_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_output(SCREEN_LCD_BL_PIN, GPIO_LEVEL_LOW);
    screen_gpio_output(SCREEN_TOUCH_RST_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_input(SCREEN_TOUCH_INT_PIN);

    uapi_pin_set_mode(SCREEN_LCD_SPI_SCK_PIN, SCREEN_LCD_SPI_PIN_MODE);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MOSI_PIN, SCREEN_LCD_SPI_PIN_MODE);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, SCREEN_LCD_SPI_PIN_MODE);

    spi_attr_t spi_attr = {0};
    spi_attr.is_slave = false;
    spi_attr.slave_num = 1;
    spi_attr.bus_clk = SCREEN_LCD_SPI_BUS_CLK;
    spi_attr.freq_mhz = SCREEN_LCD_SPI_FREQ_MHZ;
    spi_attr.clk_polarity = 0;
    spi_attr.clk_phase = 0;
    spi_attr.frame_format = 0;
    spi_attr.frame_size = HAL_SPI_FRAME_SIZE_8;
    spi_attr.spi_frame_format = 0;

    errcode_t ret = uapi_spi_init(SCREEN_LCD_SPI_BUS, &spi_attr, NULL);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    uapi_pin_set_mode(SCREEN_TOUCH_SCL_PIN, SCREEN_TOUCH_I2C_PIN_MODE);
    uapi_pin_set_mode(SCREEN_TOUCH_SDA_PIN, SCREEN_TOUCH_I2C_PIN_MODE);
    ret = uapi_i2c_master_init(SCREEN_TOUCH_I2C_BUS,
                               SCREEN_TOUCH_I2C_BAUDRATE,
                               SCREEN_TOUCH_I2C_HSCODE);
    return ret;
}

void screen_board_delay_ms(uint32_t ms)
{
    (void)uapi_tcxo_delay_ms(ms);
}

void screen_board_delay_us(uint32_t us)
{
    (void)uapi_tcxo_delay_us(us);
}

void screen_lcd_cs(bool level)
{
    uapi_gpio_set_val(SCREEN_LCD_CS_PIN, level ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

void screen_lcd_dc(bool data_mode)
{
    uapi_gpio_set_val(SCREEN_LCD_DC_PIN, data_mode ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

void screen_lcd_rst(bool level)
{
    uapi_gpio_set_val(SCREEN_LCD_RST_PIN, level ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

void screen_lcd_bl(bool level)
{
    uapi_gpio_set_val(SCREEN_LCD_BL_PIN, level ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

errcode_t screen_lcd_spi_write(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return ERRCODE_SUCC;
    }

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = (uint8_t *)data;
    xfer.tx_bytes = len;
    return uapi_spi_master_write(SCREEN_LCD_SPI_BUS, &xfer, 1000);
}

void screen_touch_rst(bool level)
{
    uapi_gpio_set_val(SCREEN_TOUCH_RST_PIN, level ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

bool screen_touch_int_level(void)
{
    return uapi_gpio_get_val(SCREEN_TOUCH_INT_PIN) == GPIO_LEVEL_HIGH;
}

errcode_t screen_touch_i2c_write(uint8_t reg, const uint8_t *data, uint32_t len)
{
    uint8_t buf[16];
    if (len > (sizeof(buf) - 1)) {
        return ERRCODE_FAIL;
    }

    buf[0] = reg;
    for (uint32_t i = 0; i < len; i++) {
        buf[i + 1] = data[i];
    }

    i2c_data_t i2c_data = {0};
    i2c_data.send_buf = buf;
    i2c_data.send_len = len + 1;
    return uapi_i2c_master_write(SCREEN_TOUCH_I2C_BUS, SCREEN_TOUCH_I2C_ADDR, &i2c_data);
}

errcode_t screen_touch_i2c_read(uint8_t reg, uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return ERRCODE_SUCC;
    }

    i2c_data_t tx = {0};
    tx.send_buf = &reg;
    tx.send_len = 1;
    errcode_t ret = uapi_i2c_master_write(SCREEN_TOUCH_I2C_BUS, SCREEN_TOUCH_I2C_ADDR, &tx);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    i2c_data_t rx = {0};
    rx.receive_buf = data;
    rx.receive_len = len;
    return uapi_i2c_master_read(SCREEN_TOUCH_I2C_BUS, SCREEN_TOUCH_I2C_ADDR, &rx);
}
