/**
 * @file sd_spi.c
 * @brief Minimal SPI-mode SD card block reader.
 */
#include "sd_spi.h"
#include "spi_bus.h"
#include "screen_config.h"

#include "gpio.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "spi.h"
#include "tcxo.h"
#include <stdio.h>

#define SD_CMD_TIMEOUT_MS       1000U
#define SD_LOCK_TIMEOUT_MS      2000U
#define SD_CMD_RETRIES          256U
#define SD_TOKEN_RETRIES        60000U
#define SD_CMD0_ATTEMPTS        24U
#define SD_INIT_CYCLES          3U
#define SD_ACMD41_ATTEMPTS      200U
#define SD_POWER_STABLE_MS      250U
#define SD_RETRY_STABLE_MS      120U

#define SD_R1_IDLE              0x01U
#define SD_R1_ILLEGAL_CMD       0x04U
#define SD_TOKEN_START_BLOCK    0xFEU

#define SD_CMD0                 0U
#define SD_CMD8                 8U
#define SD_CMD12                12U
#define SD_CMD13                13U
#define SD_CMD16                16U
#define SD_CMD17                17U
#define SD_CMD55                55U
#define SD_CMD58                58U
#define SD_ACMD41               41U

#ifndef SCREEN_SD_TEST_BITBANG_ONLY
#define SCREEN_SD_TEST_BITBANG_ONLY 0
#endif

static bool g_sd_ready;
static bool g_sd_high_capacity;
static bool g_sd_bitbang_mode;
static char g_sd_last_error[48] = "未初始化";
static uint32_t g_sd_spi_error_logs;
static uint8_t g_sd_dummy_tx[SD_SPI_SECTOR_SIZE];
static bool g_sd_dummy_tx_ready;

static void set_error(const char *text)
{
    if (text == NULL) {
        text = "未知错误";
    }
    (void)snprintf(g_sd_last_error, sizeof(g_sd_last_error), "%s", text);
}

static void sd_prepare_dummy_tx(void)
{
    if (g_sd_dummy_tx_ready) {
        return;
    }

    for (uint32_t i = 0; i < SD_SPI_SECTOR_SIZE; i++) {
        g_sd_dummy_tx[i] = 0xFFU;
    }
    g_sd_dummy_tx_ready = true;
}

static bool sd_read_fifo_byte(uint8_t *byte, bool log_error)
{
    uint8_t rx = 0xFFU;
    spi_xfer_data_t xfer = {0};
    xfer.rx_buff = &rx;
    xfer.rx_bytes = 1;

    errcode_t ret = uapi_spi_master_read(SCREEN_LCD_SPI_BUS, &xfer, 0);
    if (ret != ERRCODE_SUCC) {
        if (log_error && g_sd_spi_error_logs < 8U) {
            g_sd_spi_error_logs++;
            osal_printk("[SD] spi read-fifo fail ret=0x%x\r\n", ret);
        }
        return false;
    }

    if (byte != NULL) {
        *byte = rx;
    }
    return true;
}

static void sd_bb_delay(void)
{
    uapi_tcxo_delay_us(8);
}

#if SCREEN_SD_TEST_BITBANG_ONLY
static void sd_bitbang_begin(void)
{
    g_sd_bitbang_mode = true;

    /* Ensure both CS lines are deselected before pin mux changes */
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();

    uapi_pin_set_mode(SCREEN_LCD_SPI_SCK_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MOSI_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, HAL_PIO_FUNC_GPIO);

    /*
     * Overdrive MISO HIGH before switching to input.
     * The ILI9341's SDO may not fully tri-state when LCD_CS goes high,
     * holding the shared MISO line LOW and preventing the SD card from
     * driving a valid R1 response during CMD0.
     */
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MISO_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(SCREEN_LCD_SPI_MISO_PIN, GPIO_LEVEL_HIGH);
    uapi_tcxo_delay_us(10);

    (void)uapi_pin_set_ds(SCREEN_LCD_SPI_SCK_PIN, PIN_DS_4);
    (void)uapi_pin_set_ds(SCREEN_LCD_SPI_MOSI_PIN, PIN_DS_4);
    (void)uapi_pin_set_ds(SCREEN_SD_CS_PIN, PIN_DS_4);
    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_SCK_PIN, PIN_PULL_TYPE_DOWN);
    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MOSI_PIN, PIN_PULL_TYPE_UP);
    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_STRONG_UP);
    (void)uapi_pin_set_pull(SCREEN_SD_CS_PIN, PIN_PULL_TYPE_UP);
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    (void)uapi_pin_set_ie(SCREEN_LCD_SPI_MISO_PIN, PIN_IE_ENABLE);
#endif

    uapi_gpio_set_dir(SCREEN_LCD_SPI_SCK_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MOSI_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MISO_PIN, GPIO_DIRECTION_INPUT);

    uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
    uapi_gpio_set_val(SCREEN_LCD_SPI_MOSI_PIN, GPIO_LEVEL_HIGH);
}

static void sd_bitbang_end(void)
{
    g_sd_bitbang_mode = false;
    uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
    uapi_gpio_set_val(SCREEN_LCD_SPI_MOSI_PIN, GPIO_LEVEL_HIGH);
    uapi_pin_set_mode(SCREEN_LCD_SPI_SCK_PIN, SCREEN_LCD_SPI_PIN_MODE);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MOSI_PIN, SCREEN_LCD_SPI_PIN_MODE);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, SCREEN_LCD_SPI_PIN_MODE);
}
#endif

static void sd_restore_lcd_bus(void)
{
#if SCREEN_SD_TEST_BITBANG_ONLY
    sd_bitbang_end();
#endif
    (void)spi_bus_restore_lcd_mode();
}

static uint8_t sd_bitbang_xfer_byte(uint8_t tx)
{
    uint8_t rx = 0;

    for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
        uapi_gpio_set_val(SCREEN_LCD_SPI_MOSI_PIN, (tx & mask) ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
        sd_bb_delay();
        uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_HIGH);
        sd_bb_delay();
        rx <<= 1U;
        if (uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN) == GPIO_LEVEL_HIGH) {
            rx |= 1U;
        }
        uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
        sd_bb_delay();
    }

    uapi_gpio_set_val(SCREEN_LCD_SPI_MOSI_PIN, GPIO_LEVEL_HIGH);
    return rx;
}

static void sd_send_byte(uint8_t byte)
{
    if (g_sd_bitbang_mode) {
        (void)sd_bitbang_xfer_byte(byte);
        return;
    }

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = &byte;
    xfer.tx_bytes = 1;
    errcode_t ret = uapi_spi_master_write(SCREEN_LCD_SPI_BUS, &xfer, SD_CMD_TIMEOUT_MS);
    if (ret != ERRCODE_SUCC && g_sd_spi_error_logs < 8U) {
        g_sd_spi_error_logs++;
        osal_printk("[SD] spi write fail ret=0x%x\r\n", ret);
    }

    /* Standard SPI shifts one byte back while transmitting. Drain it so command writes do not fill RX FIFO. */
    (void)sd_read_fifo_byte(NULL, false);
}

static uint8_t sd_recv_byte(void)
{
    if (g_sd_bitbang_mode) {
        return sd_bitbang_xfer_byte(0xFFU);
    }

    uint8_t tx = 0xFFU;
    uint8_t rx = 0xFFU;
    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = &tx;
    xfer.tx_bytes = 1;
    errcode_t ret = uapi_spi_master_write(SCREEN_LCD_SPI_BUS, &xfer, SD_CMD_TIMEOUT_MS);
    if (ret == ERRCODE_SUCC && !sd_read_fifo_byte(&rx, true)) {
        rx = 0xFFU;
    }
    if (ret != ERRCODE_SUCC && g_sd_spi_error_logs < 8U) {
        g_sd_spi_error_logs++;
        osal_printk("[SD] spi clock fail ret=0x%x\r\n", ret);
    }
    return rx;
}

static bool sd_recv_block(uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0U) {
        return false;
    }

    if (g_sd_bitbang_mode) {
        for (uint32_t i = 0; i < len; i++) {
            buf[i] = sd_bitbang_xfer_byte(0xFFU);
        }
        return true;
    }

    if (len > SD_SPI_SECTOR_SIZE) {
        return false;
    }
    sd_prepare_dummy_tx();

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = g_sd_dummy_tx;
    xfer.tx_bytes = len;
    xfer.rx_buff = buf;
    xfer.rx_bytes = len;

    errcode_t ret = uapi_spi_master_writeread(SCREEN_LCD_SPI_BUS, &xfer, SD_CMD_TIMEOUT_MS);
    if (ret != ERRCODE_SUCC) {
        if (g_sd_spi_error_logs < 8U) {
            g_sd_spi_error_logs++;
            osal_printk("[SD] spi block read fail len=%lu ret=0x%x\r\n",
                        (unsigned long)len, ret);
        }
        return false;
    }
    return true;
}

static void sd_log_pin_diag(const char *stage)
{
    gpio_level_t sd_cs = uapi_gpio_get_val(SCREEN_SD_CS_PIN);
    gpio_level_t lcd_cs = uapi_gpio_get_val(SCREEN_LCD_CS_PIN);

    if (!g_sd_bitbang_mode) {
        uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, HAL_PIO_FUNC_GPIO);
        uapi_gpio_set_dir(SCREEN_LCD_SPI_MISO_PIN, GPIO_DIRECTION_INPUT);
    }
    gpio_level_t miso_gpio = uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN);
    if (!g_sd_bitbang_mode) {
        uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, SCREEN_LCD_SPI_PIN_MODE);
    }

    osal_printk("[SD] diag %s sd_cs=%u lcd_cs=%u miso_gpio=%u\r\n",
                stage, (unsigned int)sd_cs, (unsigned int)lcd_cs, (unsigned int)miso_gpio);
}

static void sd_deselect(void)
{
    spi_bus_sd_cs_high();
    sd_send_byte(0xFFU);
}

static void sd_select(void)
{
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_low();
    sd_send_byte(0xFFU);
}

static uint8_t sd_wait_r1(void)
{
    for (uint32_t i = 0; i < SD_CMD_RETRIES; i++) {
        uint8_t r = sd_recv_byte();
        if ((r & 0x80U) == 0U) {
            return r;
        }
    }
    return 0xFFU;
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, bool keep_selected)
{
    sd_deselect();
    sd_select();

    sd_send_byte((uint8_t)(0x40U | cmd));
    sd_send_byte((uint8_t)(arg >> 24));
    sd_send_byte((uint8_t)(arg >> 16));
    sd_send_byte((uint8_t)(arg >> 8));
    sd_send_byte((uint8_t)arg);
    if (cmd == SD_CMD0) {
        sd_send_byte(0x95U);
    } else if (cmd == SD_CMD8) {
        sd_send_byte(0x87U);
    } else {
        sd_send_byte(0xFFU);
    }

    uint8_t r = sd_wait_r1();
    if (!keep_selected) {
        sd_deselect();
    }
    return r;
}

static uint8_t sd_acmd(uint8_t cmd, uint32_t arg)
{
    uint8_t r = sd_cmd(SD_CMD55, 0, false);
    if (r > SD_R1_IDLE) {
        return r;
    }
    return sd_cmd(cmd, arg, false);
}

static bool sd_wait_token(uint8_t token)
{
    for (uint32_t i = 0; i < SD_TOKEN_RETRIES; i++) {
        if (sd_recv_byte() == token) {
            return true;
        }
    }
    return false;
}

static void sd_wait_not_busy(void)
{
    for (uint32_t i = 0; i < SD_CMD_RETRIES; i++) {
        if (sd_recv_byte() == 0xFFU) {
            return;
        }
    }
}

static void sd_send_idle_clocks(void)
{
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();
    for (uint32_t i = 0; i < 32U; i++) {
        sd_send_byte(0xFFU);
    }
}

static void sd_prepare_init_cycle(uint32_t cycle)
{
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();
    uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
    uapi_gpio_set_val(SCREEN_LCD_SPI_MOSI_PIN, GPIO_LEVEL_HIGH);

    osal_msleep((cycle == 0U) ? SD_POWER_STABLE_MS : SD_RETRY_STABLE_MS);
    sd_send_idle_clocks();
}

static void sd_recover_after_mcu_reset(void)
{
    /*
     * The panel reset key does not power-cycle the SD card. If the previous
     * run left the card inside an SPI transaction, park CS high first, then
     * clock both deselected and selected phases to release any pending byte.
     */
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();
    for (uint32_t i = 0; i < 64U; i++) {
        sd_send_byte(0xFFU);
    }

    spi_bus_sd_cs_low();
    for (uint32_t i = 0; i < 16U; i++) {
        sd_send_byte(0xFFU);
    }

    spi_bus_sd_cs_high();
    for (uint32_t i = 0; i < 32U; i++) {
        sd_send_byte(0xFFU);
    }

    uint8_t r = sd_cmd(SD_CMD12, 0, true);
    sd_wait_not_busy();
    sd_deselect();
    osal_printk("[SD] recover CMD12 resp=0x%02x\r\n", r);

    r = sd_cmd(SD_CMD13, 0, true);
    uint8_t status = sd_recv_byte();
    sd_deselect();
    osal_printk("[SD] recover CMD13 resp=0x%02x status=0x%02x\r\n", r, status);
}

static uint8_t sd_try_cmd0_sequence(const char *tag)
{
    uint8_t r = 0xFFU;
    uint8_t last_logged = 0xFFU;

    for (uint32_t cycle = 0; cycle < SD_INIT_CYCLES; cycle++) {
        sd_prepare_init_cycle(cycle);
        for (uint32_t attempt = 1; attempt <= SD_CMD0_ATTEMPTS; attempt++) {
            r = sd_cmd(SD_CMD0, 0, false);
            if (attempt == 1U || attempt == SD_CMD0_ATTEMPTS ||
                r == SD_R1_IDLE || r != last_logged) {
                osal_printk("[SD] CMD0 %s cycle=%u attempt=%u resp=0x%02x\r\n",
                            tag, (unsigned int)(cycle + 1U), (unsigned int)attempt, r);
                last_logged = r;
            }
            if (r == SD_R1_IDLE) {
                return r;
            }
            osal_msleep(12);
            sd_send_idle_clocks();
        }
    }

    return r;
}

errcode_t sd_spi_init_card(void)
{
    if (g_sd_ready) {
        set_error("无");
        return ERRCODE_SUCC;
    }

    g_sd_ready = false;
    g_sd_high_capacity = false;
    g_sd_spi_error_logs = 0;

    if (spi_bus_lock(SD_LOCK_TIMEOUT_MS) != ERRCODE_SUCC) {
        set_error("SPI总线忙");
        return ERRCODE_FAIL;
    }

#if SCREEN_SD_TEST_BITBANG_ONLY
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();
#else
    errcode_t ret = spi_bus_enter_sd_mode();
    if (ret != ERRCODE_SUCC) {
        set_error("SD低速SPI配置失败");
        spi_bus_unlock();
        return ret;
    }
#endif

#if SCREEN_SD_TEST_BITBANG_ONLY
    sd_bitbang_begin();
    osal_printk("[SD] init using gpio slow spi\r\n");
#else
    g_sd_bitbang_mode = false;
    osal_printk("[SD] init using hardware spi\r\n");
#endif
    sd_log_pin_diag("before-idle");

    uint8_t r = sd_try_cmd0_sequence("clean");
    if (r != SD_R1_IDLE) {
        osal_printk("[SD] clean CMD0 failed resp=0x%02x, entering recovery\r\n", r);
        sd_recover_after_mcu_reset();
        r = sd_try_cmd0_sequence("recover");
    }
    if (r != SD_R1_IDLE) {
        sd_log_pin_diag("cmd0-fail");
        set_error((r == 0xFFU) ? "SD无响应:查CS/MISO/供电" : "SD卡初始化失败");
        sd_restore_lcd_bus();
        spi_bus_unlock();
        return ERRCODE_FAIL;
    }

    bool v2_card = false;
    r = sd_cmd(SD_CMD8, 0x000001AAU, true);
    if (r == SD_R1_IDLE) {
        uint8_t r7[4];
        for (uint32_t i = 0; i < sizeof(r7); i++) {
            r7[i] = sd_recv_byte();
        }
        sd_deselect();
        osal_printk("[SD] CMD8 resp=0x%02x r7=%02x %02x %02x %02x\r\n",
                    r, r7[0], r7[1], r7[2], r7[3]);
        if (r7[2] != 0x01U || r7[3] != 0xAAU) {
            set_error("SD CMD8电压不匹配");
            sd_restore_lcd_bus();
            spi_bus_unlock();
            return ERRCODE_FAIL;
        }
        v2_card = true;
    } else {
        sd_deselect();
        osal_printk("[SD] CMD8 resp=0x%02x\r\n", r);
        if ((r & SD_R1_ILLEGAL_CMD) == 0U) {
            set_error("SD CMD8响应异常");
            sd_restore_lcd_bus();
            spi_bus_unlock();
            return ERRCODE_FAIL;
        }
    }

    uint32_t acmd_arg = v2_card ? 0x40000000U : 0U;
    for (uint32_t attempt = 0; attempt < SD_ACMD41_ATTEMPTS; attempt++) {
        r = sd_acmd(SD_ACMD41, acmd_arg);
        if (r == 0U) {
            osal_printk("[SD] ACMD41 ready attempt=%u\r\n", (unsigned int)(attempt + 1U));
            break;
        }
        osal_msleep(10);
    }
    if (r != 0U) {
        set_error("SD ACMD41超时");
        sd_restore_lcd_bus();
        spi_bus_unlock();
        return ERRCODE_FAIL;
    }

    r = sd_cmd(SD_CMD58, 0, true);
    if (r == 0U) {
        uint8_t ocr[4];
        for (uint32_t i = 0; i < sizeof(ocr); i++) {
            ocr[i] = sd_recv_byte();
        }
        g_sd_high_capacity = (ocr[0] & 0x40U) != 0U;
        osal_printk("[SD] CMD58 OCR=%02x %02x %02x %02x\r\n",
                    ocr[0], ocr[1], ocr[2], ocr[3]);
    } else {
        osal_printk("[SD] CMD58 resp=0x%02x\r\n", r);
    }
    sd_deselect();

    if (!g_sd_high_capacity) {
        r = sd_cmd(SD_CMD16, SD_SPI_SECTOR_SIZE, false);
        if (r != 0U) {
            set_error("SD CMD16失败");
            sd_restore_lcd_bus();
            spi_bus_unlock();
            return ERRCODE_FAIL;
        }
    }

#if SCREEN_SD_TEST_BITBANG_ONLY
    g_sd_ready = true;
    set_error("无");
    osal_printk("[SD] init OK type=%s mode=gpio-bitbang\r\n", g_sd_high_capacity ? "SDHC/SDXC" : "SDSC");
    sd_restore_lcd_bus();
    spi_bus_unlock();
    return ERRCODE_SUCC;
#else
    ret = spi_bus_enter_sd_fast();
    if (ret != ERRCODE_SUCC) {
        set_error("SD快速SPI配置失败");
        (void)spi_bus_restore_lcd_mode();
        spi_bus_unlock();
        return ret;
    }

    g_sd_ready = true;
    set_error("无");
    osal_printk("[SD] init OK type=%s\r\n", g_sd_high_capacity ? "SDHC/SDXC" : "SDSC");

    (void)spi_bus_restore_lcd_mode();
    spi_bus_unlock();
    return ERRCODE_SUCC;
#endif
}

errcode_t sd_spi_read_sector(uint32_t sector, uint8_t *buf, uint32_t buf_size)
{
    if (!g_sd_ready || buf == NULL || buf_size < SD_SPI_SECTOR_SIZE) {
        set_error("SD未就绪");
        return ERRCODE_FAIL;
    }

    if (spi_bus_lock(SD_LOCK_TIMEOUT_MS) != ERRCODE_SUCC) {
        set_error("SPI总线忙");
        return ERRCODE_FAIL;
    }

#if SCREEN_SD_TEST_BITBANG_ONLY
    spi_bus_lcd_cs_high();
    spi_bus_sd_cs_high();
    sd_bitbang_begin();
#else
    errcode_t ret = spi_bus_enter_sd_fast();
    if (ret != ERRCODE_SUCC) {
        set_error("SD快速SPI配置失败");
        spi_bus_unlock();
        return ret;
    }
#endif

    uint32_t addr = g_sd_high_capacity ? sector : (sector * SD_SPI_SECTOR_SIZE);
    uint8_t r = sd_cmd(SD_CMD17, addr, true);
    if (r != 0U) {
        g_sd_ready = false;
        set_error("SD CMD17失败");
        osal_printk("[SD] CMD17 fail sector=%lu addr=%lu resp=0x%02x\r\n",
                    (unsigned long)sector, (unsigned long)addr, r);
        sd_deselect();
        sd_restore_lcd_bus();
        spi_bus_unlock();
        return ERRCODE_FAIL;
    }
    if (!sd_wait_token(SD_TOKEN_START_BLOCK)) {
        g_sd_ready = false;
        set_error("SD读数据超时");
        osal_printk("[SD] CMD17 token timeout sector=%lu addr=%lu\r\n",
                    (unsigned long)sector, (unsigned long)addr);
        sd_deselect();
        sd_restore_lcd_bus();
        spi_bus_unlock();
        return ERRCODE_FAIL;
    }

    if (!sd_recv_block(buf, SD_SPI_SECTOR_SIZE)) {
        g_sd_ready = false;
        set_error("SD块读取失败");
        sd_deselect();
        sd_restore_lcd_bus();
        spi_bus_unlock();
        return ERRCODE_FAIL;
    }
    (void)sd_recv_byte();
    (void)sd_recv_byte();
    sd_deselect();

    sd_restore_lcd_bus();
    spi_bus_unlock();
    return ERRCODE_SUCC;
}

bool sd_spi_is_ready(void)
{
    return g_sd_ready;
}

const char *sd_spi_last_error(void)
{
    return g_sd_last_error;
}
