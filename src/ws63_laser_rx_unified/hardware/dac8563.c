/**
 * @file dac8563.c
 * @brief DAC8563 SPI driver for WS63.
 */
#include "dac8563.h"
#include "config.h"
#include "gpio.h"
#include "hal_gpio.h"
#include "hal_spi.h"
#include "pinctrl.h"
#include "spi.h"
#include "soc_osal.h"
#include "tcxo.h"

#ifndef DAC8563_ENABLE_SPI_DMA
#define DAC8563_ENABLE_SPI_DMA 0
#endif

#if DAC8563_ENABLE_SPI_DMA
#include "dma.h"
#endif

#define DAC_POWER_SETTLE_MS 30
#define DAC_RESET_SETTLE_MS 5
#define DAC_REF_SETTLE_MS 20
#define DAC_CMD_SETTLE_US 10
#define DAC_SPI_DMA_WIDTH 0U
#define DAC_SPI_POLL_TIMEOUT 1000U

static bool g_dac_spi_dma_ready;

static errcode_t dac8563_poll_write(const spi_xfer_data_t *xfer)
{
    /* SPI0 is dedicated to the DAC on unified RX. Keep both CS edges and the
     * FIFO drain in one short critical section, without the generic UAPI
     * parameter/mutex path on every 24-bit motion sample. */
    uint32_t irq_sts = osal_irq_lock();
    errcode_t ret = hal_gpio_output(DAC_CS_PIN, GPIO_LEVEL_LOW);
    if (ret == ERRCODE_SUCC) {
        ret = hal_spi_write(DAC_SPI_BUS, (hal_spi_xfer_data_t *)xfer,
                            DAC_SPI_POLL_TIMEOUT);
    }
    if (ret == ERRCODE_SUCC) {
        ret = hal_spi_ctrl(DAC_SPI_BUS, SPI_CTRL_CHECK_FIFO_BUSY,
                           DAC_SPI_POLL_TIMEOUT);
    }
    errcode_t cs_ret = hal_gpio_output(DAC_CS_PIN, GPIO_LEVEL_HIGH);
    osal_irq_restore(irq_sts);
    return (ret == ERRCODE_SUCC) ? cs_ret : ret;
}

static errcode_t dac8563_write_channel_raw(uint8_t cmd, uint16_t value, bool settle)
{
    uint8_t buf[3] = {
        cmd,
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
    };

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = buf;
    xfer.tx_bytes = sizeof(buf);

#if DAC8563_ENABLE_SPI_DMA
    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_LOW);
    errcode_t ret = uapi_spi_master_write(DAC_SPI_BUS, &xfer, DAC_SPI_POLL_TIMEOUT);
    uapi_gpio_set_val(DAC_CS_PIN, GPIO_LEVEL_HIGH);
    if (ret != ERRCODE_SUCC && g_dac_spi_dma_ready) {
        osal_printk("[DAC8563] spi DMA write failed ret=0x%x, disable dma\r\n", ret);
        (void)uapi_spi_set_dma_mode(DAC_SPI_BUS, false, NULL);
        g_dac_spi_dma_ready = false;
    }
#else
    errcode_t ret = dac8563_poll_write(&xfer);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[DAC8563] spi write failed cmd=0x%x ret=0x%x\r\n", cmd, ret);
    }
#endif
    if (settle) {
        (void)uapi_tcxo_delay_us(DAC_CMD_SETTLE_US);
    }
    return ret;
}

errcode_t dac8563_write_channel(uint8_t cmd, uint16_t value)
{
    return dac8563_write_channel_raw(cmd, value, true);
}

static errcode_t dac8563_configure_device(void)
{
    errcode_t ret = dac8563_write_channel(DAC_CMD_RESET, 0x0001);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    (void)uapi_tcxo_delay_ms(DAC_RESET_SETTLE_MS);
    ret = dac8563_write_channel(DAC_CMD_PWR_UP, 0x0003);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    ret = dac8563_write_channel(DAC_CMD_INT_REF_EN, 0x0001);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    (void)uapi_tcxo_delay_ms(DAC_REF_SETTLE_MS);
    ret = dac8563_write_channel(DAC_CMD_GAIN, DAC_GAIN_B2_A2);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    ret = dac8563_write_channel(DAC_CMD_LDAC_DIS, 0x0003);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    ret = dac8563_write_xy(0, 0);
    return (ret == ERRCODE_SUCC) ? dac8563_write_xy(0, 0) : ret;
}

errcode_t dac8563_init(void)
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

#if DAC8563_ENABLE_SPI_DMA && defined(CONFIG_SPI_SUPPORT_DMA) && (CONFIG_SPI_SUPPORT_DMA == 1)
    ret = uapi_dma_init();
    if (ret == ERRCODE_SUCC) {
        ret = uapi_dma_open();
    }
    if (ret == ERRCODE_SUCC) {
        spi_dma_config_t dma_cfg = {
            .src_width = DAC_SPI_DMA_WIDTH,
            .dest_width = DAC_SPI_DMA_WIDTH,
            .burst_length = 0,
            .priority = 0,
        };
        ret = uapi_spi_set_dma_mode(DAC_SPI_BUS, true, &dma_cfg);
    }
    if (ret == ERRCODE_SUCC) {
        g_dac_spi_dma_ready = true;
        osal_printk("[DAC8563] spi DMA enabled\r\n");
    } else {
        g_dac_spi_dma_ready = false;
        osal_printk("[DAC8563] spi DMA unavailable: 0x%x, fallback polling\r\n", ret);
    }
#else
    g_dac_spi_dma_ready = false;
#endif

    (void)uapi_tcxo_delay_ms(1);
    ret = dac8563_configure_device();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[DAC8563] configure failed ret=0x%x\r\n", ret);
    }
    return ret;
}

errcode_t dac8563_recover(void)
{
    return dac8563_configure_device();
}

errcode_t dac8563_write_xy(uint16_t x_val, uint16_t y_val)
{
    /* DAC8563 requires a new SYNC edge for each 24-bit frame. Preload A,
     * then write B and update both outputs together on the second frame. */
    errcode_t ret = dac8563_write_channel_raw(DAC_CMD_WRITE_A_INPUT, x_val, false);
    return (ret == ERRCODE_SUCC) ?
        dac8563_write_channel_raw(DAC_CMD_WRITE_B_UPDATE_ALL, y_val, false) : ret;
}
