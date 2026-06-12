/**
 * @file screen_board.c
 * @brief WS63 hardware adapter for the ST7796 + FT6336 screen board.
 */
#include "screen_board.h"
#include "screen_config.h"

#include "gpio.h"
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

    /* Initialize software I2C pins (open-drain with pull-up) */
    uapi_pin_set_mode(SCREEN_TOUCH_SCL_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_TOUCH_SDA_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_pull(SCREEN_TOUCH_SCL_PIN, PIN_PULL_TYPE_UP);
    uapi_pin_set_pull(SCREEN_TOUCH_SDA_PIN, PIN_PULL_TYPE_UP);
    uapi_gpio_set_dir(SCREEN_TOUCH_SCL_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_TOUCH_SDA_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(SCREEN_TOUCH_SCL_PIN, GPIO_LEVEL_HIGH);
    uapi_gpio_set_val(SCREEN_TOUCH_SDA_PIN, GPIO_LEVEL_HIGH);

    return ERRCODE_SUCC;
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

/* Software I2C (bit-bang) implementation */
static inline void i2c_sda_high(void)
{
    uapi_gpio_set_val(SCREEN_TOUCH_SDA_PIN, GPIO_LEVEL_HIGH);
}

static inline void i2c_sda_low(void)
{
    uapi_gpio_set_val(SCREEN_TOUCH_SDA_PIN, GPIO_LEVEL_LOW);
}

static inline void i2c_scl_high(void)
{
    uapi_gpio_set_val(SCREEN_TOUCH_SCL_PIN, GPIO_LEVEL_HIGH);
}

static inline void i2c_scl_low(void)
{
    uapi_gpio_set_val(SCREEN_TOUCH_SCL_PIN, GPIO_LEVEL_LOW);
}

static inline bool i2c_sda_read(void)
{
    return uapi_gpio_get_val(SCREEN_TOUCH_SDA_PIN) == GPIO_LEVEL_HIGH;
}

static inline void i2c_delay(void)
{
    screen_board_delay_us(SCREEN_TOUCH_I2C_DELAY_US);
}

static void i2c_start(void)
{
    i2c_sda_high();
    i2c_scl_high();
    i2c_delay();
    i2c_sda_low();
    i2c_delay();
    i2c_scl_low();
    i2c_delay();
}

static void i2c_stop(void)
{
    i2c_sda_low();
    i2c_scl_high();
    i2c_delay();
    i2c_sda_high();
    i2c_delay();
}

static bool i2c_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        if (byte & 0x80) {
            i2c_sda_high();
        } else {
            i2c_sda_low();
        }
        byte <<= 1;
        i2c_scl_high();
        i2c_delay();
        i2c_scl_low();
        i2c_delay();
    }
    /* Read ACK */
    i2c_sda_high(); /* release SDA */
    i2c_scl_high();
    i2c_delay();
    bool ack = !i2c_sda_read();
    i2c_scl_low();
    i2c_delay();
    return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
    uint8_t byte = 0;
    i2c_sda_high(); /* release SDA */
    for (int i = 0; i < 8; i++) {
        byte <<= 1;
        i2c_scl_high();
        i2c_delay();
        if (i2c_sda_read()) {
            byte |= 1;
        }
        i2c_scl_low();
        i2c_delay();
    }
    /* Send ACK/NACK */
    if (ack) {
        i2c_sda_low();
    } else {
        i2c_sda_high();
    }
    i2c_scl_high();
    i2c_delay();
    i2c_scl_low();
    i2c_delay();
    return byte;
}

errcode_t screen_touch_i2c_write(uint8_t reg, const uint8_t *data, uint32_t len)
{
    i2c_start();
    /* Send device address + write bit */
    if (!i2c_write_byte((SCREEN_TOUCH_I2C_ADDR << 1) | 0)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    /* Send register address */
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    /* Send data */
    for (uint32_t i = 0; i < len; i++) {
        if (!i2c_write_byte(data[i])) {
            i2c_stop();
            return ERRCODE_FAIL;
        }
    }
    i2c_stop();
    return ERRCODE_SUCC;
}

errcode_t screen_touch_i2c_read(uint8_t reg, uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return ERRCODE_SUCC;
    }

    i2c_start();
    /* Send device address + write bit */
    if (!i2c_write_byte((SCREEN_TOUCH_I2C_ADDR << 1) | 0)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    /* Send register address */
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    /* Repeated start */
    i2c_start();
    /* Send device address + read bit */
    if (!i2c_write_byte((SCREEN_TOUCH_I2C_ADDR << 1) | 1)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    /* Read data */
    for (uint32_t i = 0; i < len; i++) {
        bool ack = (i < len - 1); /* ACK all except last byte */
        data[i] = i2c_read_byte(ack);
    }
    i2c_stop();
    return ERRCODE_SUCC;
}
