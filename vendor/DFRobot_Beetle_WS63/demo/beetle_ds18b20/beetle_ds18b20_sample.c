/**！
 * @file beetle_ds18b20_sample.c
 * @brief Implementing the OneWire protocol via GPIO to drive the DS18B20.
 * @copyright  Copyright (c) 2025 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license     The MIT License (MIT)
 * @author [Martin](Martin@dfrobot.com)
 * @version  V1.0
 * @date  2025-9-29
 */

#include "pinctrl.h"
#include "gpio.h"
#include "systick.h"
#include "soc_osal.h"
#include "app_init.h"

#include "tcxo.h"

#define DS_TASK_PRIO 24
#define DS_TASK_STACK_SIZE 0x1000
#define READITV 10
#define TASKITV 1000

static inline uint32_t get_time_us(void)
{
    return (uint32_t)uapi_tcxo_get_us();
}

static void delay_us(uint32_t us)
{
    uapi_tcxo_delay_us(us);
}

static inline void bus_drive_low(void)
{
    uapi_gpio_set_dir(CONFIG_ONEWIRE_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(CONFIG_ONEWIRE_PIN, GPIO_LEVEL_LOW);
}

static inline void bus_release(void)
{
    uapi_gpio_set_dir(CONFIG_ONEWIRE_PIN, GPIO_DIRECTION_INPUT);
}

static inline uint8_t bus_read_level(void)
{
    return (uint8_t)uapi_gpio_get_val(CONFIG_ONEWIRE_PIN);
}

static bool ow_reset(void)
{
    bus_drive_low();
    delay_us(480);
    bus_release();
    delay_us(70);
    bool present = (bus_read_level() == 0);
    delay_us(410);
    return present;
}

static void ow_write_bit(uint8_t bit)
{
    if (bit) {
        bus_drive_low();
        delay_us(6);
        bus_release();
        delay_us(64);
    } else {
        bus_drive_low();
        delay_us(60);
        bus_release();
        delay_us(10);
    }
}

static uint8_t ow_read_bit(void)
{
    uint8_t bit;
    bus_drive_low();
    delay_us(6);
    bus_release();
    delay_us(9);
    bit = bus_read_level();
    delay_us(55);
    return bit;
}

static void ow_write_byte(uint8_t val)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(val & 0x01);
        val >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v >>= 1;
        if (ow_read_bit()) {
            v |= 0x80;
        }
    }
    return v;
}

static bool ds18b20_read_temp_raw(int16_t *temp_raw)
{
    if (!ow_reset()) {
        return false;
    }
    ow_write_byte(0xCC); // SKIP ROM
    ow_write_byte(0x44); // CONVERT T

    uint32_t start = get_time_us();
    while ((uint32_t)(get_time_us() - start) < 800000) {
        if (ow_read_bit()) {
            break;
        }
        osal_msleep(READITV);
    }

    if (!ow_reset()) {
        return false;
    }
    ow_write_byte(0xCC);
    ow_write_byte(0xBE); // READ SCRATCHPAD

    uint8_t temp_l = ow_read_byte();
    uint8_t temp_h = ow_read_byte();
    *temp_raw = (int16_t)((temp_h << 8) | temp_l);
    return true;
}

static float ds18b20_raw_to_celsius(int16_t raw)
{
    return (float)raw / 16.0f;
}

static int ds18b20_task(const char *arg)
{
    unused(arg);

    uapi_pin_set_mode(CONFIG_ONEWIRE_PIN, HAL_PIO_FUNC_GPIO);
    uapi_pin_set_pull(CONFIG_ONEWIRE_PIN, PIN_PULL_TYPE_UP);
    uapi_gpio_init();
    uapi_systick_init();

    static char templine[32] = {0};
    while (1) {
        int16_t raw = 0;
        bool ok = ds18b20_read_temp_raw(&raw);
        if (ok) {
            float t = ds18b20_raw_to_celsius(raw);
            sprintf(templine, "DS18B20: %.2f C\r\n", t);
            printf(templine);
        } else {
            osal_printk("DS18B20: no presence\r\n");
        }
        osal_msleep(TASKITV);
    }

    return 0;
}

static void ds18b20_entry(void)
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)ds18b20_task, 0, "DS18B20Task", DS_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, DS_TASK_PRIO);
        osal_kfree(task_handle);
    }
    osal_kthread_unlock();
}

app_run(ds18b20_entry);