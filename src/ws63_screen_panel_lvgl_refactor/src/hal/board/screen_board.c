/**
 * @file screen_board.c
 * @brief WS63 hardware adapter for the ST7796 + FT6336 screen board.
 */
#include "screen_board.h"
#include "screen_config.h"

#include "gpio.h"
#include "hal_dma.h"
#include "pinctrl.h"
#include "pwm.h"
#include "soc_osal.h"
#include "spi.h"
#include "spi_bus.h"
#include "tcxo.h"

#if SCREEN_BOARD_REV_FINAL_HW_I2C || SCREEN_BOARD_REV_FLYWIRE_HW_I2C
#include "i2c.h"
#endif

#define SCREEN_LCD_BL_PWM_CHANNEL       5U
#define SCREEN_LCD_BL_PWM_GROUP         0U
#define SCREEN_LCD_BL_PWM_PIN_MODE      PIN_MODE_1
#define SCREEN_LCD_BL_PWM_PERIOD_TICKS  1000U
#define SCREEN_LCD_BL_PWM_CYCLES        0xFFU
#define SCREEN_LCD_SPI_DMA_WIDTH        0U

static bool g_lcd_bl_pwm_ready;
static bool g_lcd_spi_dma_ready;
static bool g_lcd_bus_transaction_active;

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
    /* LCD GPIOs */
    screen_gpio_output(SCREEN_LCD_CS_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_output(SCREEN_LCD_DC_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_output(SCREEN_LCD_RST_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_output(SCREEN_LCD_BL_PIN, GPIO_LEVEL_HIGH);

    /* Touch RST/INT */
    screen_gpio_output(SCREEN_TOUCH_RST_PIN, GPIO_LEVEL_HIGH);
    screen_gpio_input(SCREEN_TOUCH_INT_PIN);

    /* SPI pinmux */
    uapi_pin_set_mode(SCREEN_LCD_SPI_SCK_PIN, SCREEN_LCD_SPI_PIN_MODE);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MOSI_PIN, SCREEN_LCD_SPI_PIN_MODE);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, SCREEN_LCD_SPI_PIN_MODE);

    spi_attr_t spi_attr = {0};
    spi_attr.is_slave = false;
    spi_attr.slave_num = 1;
    spi_attr.bus_clk = SCREEN_LCD_SPI_BUS_CLK;
    spi_attr.freq_mhz = SCREEN_LCD_SPI_BAUDRATE / 1000000;
    spi_attr.clk_polarity = 0;
    spi_attr.clk_phase = 0;
    spi_attr.frame_format = 0;
    spi_attr.frame_size = HAL_SPI_FRAME_SIZE_8;
    spi_attr.spi_frame_format = 0;

    errcode_t ret = uapi_spi_init(SCREEN_LCD_SPI_BUS, &spi_attr, NULL);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SCREEN] spi init FAILED (0x%x)\r\n", ret);
        return ret;
    }

#if defined(CONFIG_SPI_SUPPORT_DMA) && (CONFIG_SPI_SUPPORT_DMA == 1)
    ret = uapi_dma_init();
    if (ret == ERRCODE_SUCC) {
        ret = uapi_dma_open();
    }
    if (ret == ERRCODE_SUCC) {
        spi_dma_config_t dma_cfg = {
            .src_width = SCREEN_LCD_SPI_DMA_WIDTH,
            .dest_width = SCREEN_LCD_SPI_DMA_WIDTH,
            .burst_length = 0,
            .priority = 0,
        };
        ret = uapi_spi_set_dma_mode(SCREEN_LCD_SPI_BUS, true, &dma_cfg);
    }
    if (ret == ERRCODE_SUCC) {
        g_lcd_spi_dma_ready = true;
        osal_printk("[SCREEN] lcd spi DMA enabled\r\n");
    } else {
        g_lcd_spi_dma_ready = false;
        osal_printk("[SCREEN] lcd spi DMA unavailable: 0x%x, fallback polling\r\n", ret);
    }
#else
    g_lcd_spi_dma_ready = false;
#endif

#if SCREEN_BOARD_REV_FINAL_HW_I2C || SCREEN_BOARD_REV_FLYWIRE_HW_I2C
    /* Hardware I2C1: pinmux handled in screen_hw_i2c_init() */
#else
    /* Software I2C pins (open-drain with pull-up) */
    uapi_pin_set_mode(SCREEN_TOUCH_SCL_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_TOUCH_SDA_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_pull(SCREEN_TOUCH_SCL_PIN, PIN_PULL_TYPE_UP);
    uapi_pin_set_pull(SCREEN_TOUCH_SDA_PIN, PIN_PULL_TYPE_UP);
    uapi_gpio_set_dir(SCREEN_TOUCH_SCL_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_TOUCH_SDA_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(SCREEN_TOUCH_SCL_PIN, GPIO_LEVEL_HIGH);
    uapi_gpio_set_val(SCREEN_TOUCH_SDA_PIN, GPIO_LEVEL_HIGH);
#endif

#if defined(SCREEN_PANEL_ENABLE_SD) && SCREEN_PANEL_ENABLE_SD
    /* SD_CS */
    screen_gpio_output(SCREEN_SD_CS_PIN, GPIO_LEVEL_HIGH);
#endif

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

static void screen_lcd_bl_pwm_config(uint8_t brightness_pct, pwm_config_t *cfg)
{
    uint32_t high_time = (SCREEN_LCD_BL_PWM_PERIOD_TICKS * brightness_pct) / 100U;

    cfg->low_time = SCREEN_LCD_BL_PWM_PERIOD_TICKS - high_time;
    cfg->high_time = high_time;
    cfg->offset_time = 0;
    cfg->cycles = SCREEN_LCD_BL_PWM_CYCLES;
    cfg->repeat = true;
}

errcode_t screen_lcd_bl_pwm_init(uint8_t brightness_pct)
{
    if (brightness_pct > 100U) {
        brightness_pct = 100U;
    }

    pwm_config_t cfg;
    screen_lcd_bl_pwm_config(brightness_pct, &cfg);

    errcode_t ret = uapi_pin_set_mode(SCREEN_LCD_BL_PIN, SCREEN_LCD_BL_PWM_PIN_MODE);
    if (ret != ERRCODE_SUCC) {
        goto fallback;
    }

    ret = uapi_pwm_init();
    if (ret != ERRCODE_SUCC) {
        goto fallback;
    }

    ret = uapi_pwm_open(SCREEN_LCD_BL_PWM_CHANNEL, &cfg);
    if (ret != ERRCODE_SUCC) {
        goto fallback;
    }

#if defined(CONFIG_PWM_USING_V151)
    {
        uint8_t channel = SCREEN_LCD_BL_PWM_CHANNEL;
        ret = uapi_pwm_set_group(SCREEN_LCD_BL_PWM_GROUP, &channel, 1);
        if (ret == ERRCODE_SUCC) {
            ret = uapi_pwm_start_group(SCREEN_LCD_BL_PWM_GROUP);
        }
    }
#else
    ret = uapi_pwm_start(SCREEN_LCD_BL_PWM_CHANNEL);
#endif
    if (ret != ERRCODE_SUCC) {
        goto fallback;
    }

    g_lcd_bl_pwm_ready = true;
    return ERRCODE_SUCC;

fallback:
    g_lcd_bl_pwm_ready = false;
    uapi_pwm_deinit();
    screen_gpio_output(SCREEN_LCD_BL_PIN, GPIO_LEVEL_HIGH);
    osal_printk("[SCREEN] lcd bl PWM5 init failed: 0x%x, fallback full on\r\n", ret);
    return ret;
}

errcode_t screen_lcd_bl_set_brightness(uint8_t brightness_pct)
{
    if (!g_lcd_bl_pwm_ready) {
        return ERRCODE_FAIL;
    }
    if (brightness_pct > 100U) {
        brightness_pct = 100U;
    }

    pwm_config_t cfg;
    screen_lcd_bl_pwm_config(brightness_pct, &cfg);

#if defined(CONFIG_PWM_USING_V151)
    return uapi_pwm_update_cfg(SCREEN_LCD_BL_PWM_CHANNEL, &cfg);
#else
    return uapi_pwm_update_duty_ratio(SCREEN_LCD_BL_PWM_CHANNEL,
                                      cfg.low_time, cfg.high_time);
#endif
}

errcode_t screen_lcd_bus_begin(uint32_t timeout_ms)
{
    if (g_lcd_bus_transaction_active) {
        return ERRCODE_FAIL;
    }

    errcode_t ret = spi_bus_lock(timeout_ms);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    ret = spi_bus_restore_lcd_mode();
    if (ret != ERRCODE_SUCC) {
        spi_bus_unlock();
        return ret;
    }

#if defined(SCREEN_PANEL_ENABLE_SD) && SCREEN_PANEL_ENABLE_SD
    spi_bus_sd_cs_high();
#endif
    screen_lcd_cs(true);
    g_lcd_bus_transaction_active = true;
    return ERRCODE_SUCC;
}

void screen_lcd_bus_end(void)
{
    if (!g_lcd_bus_transaction_active) {
        return;
    }

    screen_lcd_cs(true);
    g_lcd_bus_transaction_active = false;
    spi_bus_unlock();
}

errcode_t screen_lcd_spi_write(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return ERRCODE_SUCC;
    }

    bool owns_bus = !g_lcd_bus_transaction_active;
    errcode_t ret = ERRCODE_SUCC;
    if (owns_bus) {
        ret = spi_bus_lock(1000);
        if (ret != ERRCODE_SUCC) {
            screen_lcd_cs(true);
            return ret;
        }
    }

    /*
     * Keep the transaction self-contained at board layer so an error path or a
     * high-level caller cannot leave LCD_CS asserted.
     */
#if defined(SCREEN_PANEL_ENABLE_SD) && SCREEN_PANEL_ENABLE_SD
    spi_bus_sd_cs_high();
#endif
    screen_lcd_cs(false);

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = (uint8_t *)data;
    xfer.tx_bytes = len;
    ret = uapi_spi_master_write(SCREEN_LCD_SPI_BUS, &xfer, 1000);
    if (ret != ERRCODE_SUCC && g_lcd_spi_dma_ready) {
        osal_printk("[SCREEN] spi DMA write failed len=%u ret=0x%x\r\n", (unsigned int)len, ret);
    }
    if (owns_bus) {
        screen_lcd_cs(true);
        spi_bus_unlock();
    }
    return ret;
}

void screen_touch_rst(bool level)
{
    uapi_gpio_set_val(SCREEN_TOUCH_RST_PIN, level ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

bool screen_touch_int_level(void)
{
    return uapi_gpio_get_val(SCREEN_TOUCH_INT_PIN) == GPIO_LEVEL_HIGH;
}

/* ========================================================================
 * Hardware I2C1 implementation (final board / flywire board)
 * ======================================================================== */
#if SCREEN_BOARD_REV_FINAL_HW_I2C || SCREEN_BOARD_REV_FLYWIRE_HW_I2C

errcode_t screen_hw_i2c_init(void)
{
    uapi_pin_set_mode(SCREEN_TOUCH_SCL_PIN, SCREEN_TOUCH_I2C_PIN_MODE);
    uapi_pin_set_mode(SCREEN_TOUCH_SDA_PIN, SCREEN_TOUCH_I2C_PIN_MODE);
    return uapi_i2c_master_init(SCREEN_TOUCH_I2C_BUS_ID,
                                SCREEN_TOUCH_I2C_BAUDRATE, 0);
}

bool screen_hw_i2c_probe(uint8_t addr_7bit)
{
    uint8_t dummy = 0;
    i2c_data_t data = {0};
    data.send_buf = &dummy;
    data.send_len = 0;
    return uapi_i2c_master_write(SCREEN_TOUCH_I2C_BUS_ID,
                                 addr_7bit, &data) == ERRCODE_SUCC;
}

errcode_t screen_hw_i2c_read_reg(uint8_t addr_7bit, uint8_t reg,
                                 uint8_t *buf, uint32_t len)
{
    i2c_data_t data = {0};
    data.send_buf = &reg;
    data.send_len = 1;
    data.receive_buf = buf;
    data.receive_len = len;
    return uapi_i2c_master_writeread(SCREEN_TOUCH_I2C_BUS_ID,
                                     addr_7bit, &data);
}

/* ========================================================================
 * Software bit-bang I2C implementation (original board)
 * ======================================================================== */
#else

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
    i2c_sda_high();
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
    i2c_sda_high();
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
    if (!i2c_write_byte((SCREEN_TOUCH_I2C_ADDR << 1) | 0)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
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
    if (!i2c_write_byte((SCREEN_TOUCH_I2C_ADDR << 1) | 0)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    i2c_start();
    if (!i2c_write_byte((SCREEN_TOUCH_I2C_ADDR << 1) | 1)) {
        i2c_stop();
        return ERRCODE_FAIL;
    }
    for (uint32_t i = 0; i < len; i++) {
        bool ack = (i < len - 1);
        data[i] = i2c_read_byte(ack);
    }
    i2c_stop();
    return ERRCODE_SUCC;
}

void screen_i2c_scan(void)
{
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        i2c_start();
        (void)i2c_write_byte((addr << 1) | 0);
        i2c_stop();
    }
}

bool screen_i2c_probe(uint8_t addr_7bit)
{
    i2c_start();
    bool ack = i2c_write_byte((addr_7bit << 1) | 0);
    i2c_stop();
    return ack;
}

#endif /* SCREEN_BOARD_REV_FINAL_HW_I2C || SCREEN_BOARD_REV_FLYWIRE_HW_I2C */
