/**
 * @file spi_bus.c
 * @brief SPI bus arbitration for LCD access.
 */
#include "spi_bus.h"

#include "screen_config.h"

#include "gpio.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "spi.h"

static osal_mutex g_spi_bus_mutex;
static bool g_spi_bus_ready;

#define SCREEN_SD_INIT_SPI_HZ 1000000U
#define SCREEN_SD_FAST_SPI_HZ 8000000U

#ifndef SCREEN_PANEL_ENABLE_SD
#define SCREEN_PANEL_ENABLE_SD 1
#endif

void spi_bus_park_pins_for_boot(void)
{
    uapi_pin_set_mode(SCREEN_LCD_CS_PIN, HAL_PIO_FUNC_GPIO);
#if SCREEN_PANEL_ENABLE_SD
    uapi_pin_set_mode(SCREEN_SD_CS_PIN, HAL_PIO_FUNC_GPIO);
#endif
    uapi_pin_set_mode(SCREEN_LCD_SPI_SCK_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MOSI_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, HAL_PIO_FUNC_GPIO);

    (void)uapi_pin_set_pull(SCREEN_LCD_CS_PIN, PIN_PULL_TYPE_UP);
#if SCREEN_PANEL_ENABLE_SD
    (void)uapi_pin_set_pull(SCREEN_SD_CS_PIN, PIN_PULL_TYPE_UP);
#endif
    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_SCK_PIN, PIN_PULL_TYPE_DOWN);
    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MOSI_PIN, PIN_PULL_TYPE_UP);
    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_STRONG_UP);

    uapi_gpio_set_dir(SCREEN_LCD_CS_PIN, GPIO_DIRECTION_OUTPUT);
#if SCREEN_PANEL_ENABLE_SD
    uapi_gpio_set_dir(SCREEN_SD_CS_PIN, GPIO_DIRECTION_OUTPUT);
#endif
    uapi_gpio_set_dir(SCREEN_LCD_SPI_SCK_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MOSI_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MISO_PIN, GPIO_DIRECTION_INPUT);

    uapi_gpio_set_val(SCREEN_LCD_CS_PIN, GPIO_LEVEL_HIGH);
#if SCREEN_PANEL_ENABLE_SD
    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_HIGH);
#endif
    uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
    uapi_gpio_set_val(SCREEN_LCD_SPI_MOSI_PIN, GPIO_LEVEL_HIGH);
}

static void spi_bus_set_dma(bool enable)
{
#if defined(CONFIG_SPI_SUPPORT_DMA) && (CONFIG_SPI_SUPPORT_DMA == 1)
    if (enable) {
        spi_dma_config_t dma_cfg = {
            .src_width = 0,
            .dest_width = 0,
            .burst_length = 0,
            .priority = 0,
        };
        (void)uapi_spi_set_dma_mode(SCREEN_LCD_SPI_BUS, true, &dma_cfg);
    } else {
        (void)uapi_spi_set_dma_mode(SCREEN_LCD_SPI_BUS, false, NULL);
    }
#else
    (void)enable;
#endif
}

static errcode_t spi_bus_set_clk(uint32_t hz)
{
    spi_attr_t attr = {0};
    uint32_t freq_mhz = (hz + 999999U) / 1000000U;
    if (freq_mhz == 0U) {
        freq_mhz = 1U;
    }

    attr.is_slave = false;
    attr.slave_num = 1;
    attr.bus_clk = SCREEN_LCD_SPI_BUS_CLK;
    attr.freq_mhz = freq_mhz;
    attr.clk_polarity = 0;
    attr.clk_phase = 0;
    attr.frame_format = 0;
    attr.frame_size = HAL_SPI_FRAME_SIZE_8;
    attr.spi_frame_format = 0;
    return uapi_spi_set_attr(SCREEN_LCD_SPI_BUS, &attr);
}

errcode_t spi_bus_init(void)
{
    if (g_spi_bus_ready) {
        return ERRCODE_SUCC;
    }

    if (osal_mutex_init(&g_spi_bus_mutex) != OSAL_SUCCESS) {
        osal_printk("[SCREEN] spi bus mutex init failed\r\n");
        return ERRCODE_FAIL;
    }

#if SCREEN_PANEL_ENABLE_SD
    uapi_pin_set_mode(SCREEN_SD_CS_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(SCREEN_SD_CS_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_HIGH);
#endif

    g_spi_bus_ready = true;
    return ERRCODE_SUCC;
}

errcode_t spi_bus_lock(uint32_t timeout_ms)
{
    if (!g_spi_bus_ready) {
        return ERRCODE_FAIL;
    }

    if (osal_mutex_lock_timeout(&g_spi_bus_mutex, timeout_ms) != OSAL_SUCCESS) {
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCC;
}

void spi_bus_unlock(void)
{
    if (!g_spi_bus_ready) {
        return;
    }

    osal_mutex_unlock(&g_spi_bus_mutex);
}

errcode_t spi_bus_enter_sd_mode(void)
{
#if SCREEN_PANEL_ENABLE_SD
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();
    spi_bus_set_dma(false);
    return spi_bus_set_clk(SCREEN_SD_INIT_SPI_HZ);
#else
    return ERRCODE_FAIL;
#endif
}

errcode_t spi_bus_enter_sd_fast(void)
{
#if SCREEN_PANEL_ENABLE_SD
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();
    spi_bus_set_dma(false);
    return spi_bus_set_clk(SCREEN_SD_FAST_SPI_HZ);
#else
    return ERRCODE_FAIL;
#endif
}

errcode_t spi_bus_restore_lcd_mode(void)
{
#if SCREEN_PANEL_ENABLE_SD
    spi_bus_sd_cs_high();
#endif
    spi_bus_lcd_cs_high();
    errcode_t ret = spi_bus_set_clk(SCREEN_LCD_SPI_BAUDRATE);
    spi_bus_set_dma(true);
    return ret;
}

void spi_bus_sd_cs_low(void)
{
#if SCREEN_PANEL_ENABLE_SD
    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_LOW);
#endif
}

void spi_bus_sd_cs_high(void)
{
#if SCREEN_PANEL_ENABLE_SD
    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_HIGH);
#endif
}

void spi_bus_lcd_cs_high(void)
{
    uapi_gpio_set_val(SCREEN_LCD_CS_PIN, GPIO_LEVEL_HIGH);
}
