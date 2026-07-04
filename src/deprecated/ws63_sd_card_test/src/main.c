/**
 * @file main.c
 * @brief Standalone SD-card/FAT read test for the MSP3223 shared SPI pins.
 */
#include "app_init.h"
#include "fat_reader.h"
#include "gpio.h"
#include "pinctrl.h"
#include "screen_config.h"
#include "sd_spi.h"
#include "soc_osal.h"
#include "spi_bus.h"

#define SD_TEST_BOOT_DELAY_MS      1000U
#define SD_TEST_RETRY_INTERVAL_MS  3000U
#define SD_TEST_TASK_PRIO          24
#define SD_TEST_TASK_STACK_SIZE    0x1800

static void sd_test_clock_cycles(uint32_t cycles)
{
    for (uint32_t i = 0; i < cycles; i++) {
        uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
        osal_udelay(8);
        uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_HIGH);
        osal_udelay(8);
    }
    uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
}

static void sd_test_log_gpio(const char *stage)
{
    uapi_pin_set_mode(SCREEN_LCD_CS_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_SD_CS_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_LCD_SPI_SCK_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MOSI_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, HAL_PIO_FUNC_GPIO);
    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_STRONG_UP);
    uapi_gpio_set_dir(SCREEN_LCD_CS_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_SD_CS_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_SCK_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MOSI_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MISO_PIN, GPIO_DIRECTION_INPUT);
    uapi_gpio_set_val(SCREEN_LCD_CS_PIN, GPIO_LEVEL_HIGH);
    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_HIGH);
    uapi_gpio_set_val(SCREEN_LCD_SPI_SCK_PIN, GPIO_LEVEL_LOW);
    uapi_gpio_set_val(SCREEN_LCD_SPI_MOSI_PIN, GPIO_LEVEL_HIGH);

    osal_printk("[SD_TEST] gpio %s lcd_cs=%u sd_cs=%u sck=%u mosi=%u miso=%u\r\n",
                stage,
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_CS_PIN),
                (unsigned int)uapi_gpio_get_val(SCREEN_SD_CS_PIN),
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_SPI_SCK_PIN),
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_SPI_MOSI_PIN),
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN));
}

static void sd_test_miso_pull_probe(void)
{
    uapi_pin_set_mode(SCREEN_LCD_SPI_MISO_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(SCREEN_LCD_SPI_MISO_PIN, GPIO_DIRECTION_INPUT);

    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_DISABLE);
    osal_msleep(5);
    gpio_level_t none = uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN);

    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_UP);
    osal_msleep(5);
    gpio_level_t weak_up = uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN);

    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_STRONG_UP);
    osal_msleep(5);
    gpio_level_t strong_up = uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN);

    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_DOWN);
    osal_msleep(5);
    gpio_level_t down = uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN);

    (void)uapi_pin_set_pull(SCREEN_LCD_SPI_MISO_PIN, PIN_PULL_TYPE_STRONG_UP);
    osal_printk("[SD_TEST] miso_pull none=%u weak_up=%u strong_up=%u down=%u\r\n",
                (unsigned int)none,
                (unsigned int)weak_up,
                (unsigned int)strong_up,
                (unsigned int)down);
}

static void sd_test_physical_diag(void)
{
    osal_printk("[SD_TEST] physical diag start\r\n");
    spi_bus_park_pins_for_boot();
    sd_test_log_gpio("diag-cs-high");
    sd_test_miso_pull_probe();

    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_HIGH);
    sd_test_clock_cycles(80);
    osal_printk("[SD_TEST] diag after 80 clocks with cs=1 miso=%u\r\n",
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN));

    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_LOW);
    osal_msleep(10);
    osal_printk("[SD_TEST] diag cs=0 miso=%u\r\n",
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN));

    sd_test_clock_cycles(16);
    osal_printk("[SD_TEST] diag after 16 clocks with cs=0 miso=%u\r\n",
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN));

    uapi_gpio_set_val(SCREEN_SD_CS_PIN, GPIO_LEVEL_HIGH);
    sd_test_clock_cycles(16);
    osal_printk("[SD_TEST] diag release cs=1 miso=%u\r\n",
                (unsigned int)uapi_gpio_get_val(SCREEN_LCD_SPI_MISO_PIN));
    osal_printk("[SD_TEST] physical diag end\r\n");
}

static void sd_test_print_files(void)
{
    uint8_t count = fat_reader_file_count();
    osal_printk("[SD_TEST] FAT mounted, gcode_files=%u\r\n", (unsigned int)count);

    if (count == 0U) {
        osal_printk("[SD_TEST] no .gcode/.nc/.gco file in root directory\r\n");
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        const fat_reader_file_t *file = fat_reader_get_file(i);
        if (file == NULL) {
            continue;
        }
        osal_printk("[SD_TEST] file[%u] name=%s size=%lu cluster=%lu\r\n",
                    (unsigned int)i,
                    file->name,
                    (unsigned long)file->size_bytes,
                    (unsigned long)file->first_cluster);
    }
}

static int sd_test_task(const char *arg)
{
    unused(arg);

    osal_printk("[SD_TEST] standalone SD card test boot\r\n");
    osal_printk("[SD_TEST] LCD/LVGL/touch/SLE are intentionally not started\r\n");

    spi_bus_park_pins_for_boot();
    sd_test_log_gpio("parked");
    sd_test_physical_diag();
    osal_msleep(SD_TEST_BOOT_DELAY_MS);

    if (spi_bus_init() != ERRCODE_SUCC) {
        osal_printk("[SD_TEST] spi_bus_init failed\r\n");
    }

    while (1) {
        sd_test_log_gpio("before-mount");

        errcode_t ret = fat_reader_mount();
        if (ret == ERRCODE_SUCC) {
            sd_test_print_files();
        } else {
            osal_printk("[SD_TEST] mount failed fat=%s sd=%s ready=%u\r\n",
                        fat_reader_last_error(),
                        sd_spi_last_error(),
                        (unsigned int)sd_spi_is_ready());
        }

        spi_bus_lcd_cs_high();
        spi_bus_sd_cs_high();
        osal_msleep(SD_TEST_RETRY_INTERVAL_MS);
    }

    return 0;
}

static void sd_test_entry(void)
{
    osal_task *task_handle = NULL;

    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)sd_test_task, 0,
                                      "SdTestTask", SD_TEST_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SD_TEST_TASK_PRIO);
        osal_kfree(task_handle);
    }
    osal_kthread_unlock();
}

app_run(sd_test_entry);
