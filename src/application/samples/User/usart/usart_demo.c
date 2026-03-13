/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: USART Sample - User defined.
 */

#include "pinctrl.h"
#include "uart.h"
#include "watchdog.h"
#include "soc_osal.h"
#include "app_init.h"

#ifdef CONFIG_SAMPLE_SUPPORT_MY_USART

/* ---------- 宏定义 ---------- */
#define USART_BAUDRATE 115200
#define USART_TRANSFER_SIZE 256
#define USART_TASK_PRIO 24
#define USART_TASK_STACK_SIZE 0x1000
#define USART_INT_WAIT_MS 5

/* 映射 Kconfig 变量及缺省值保护 */
#ifndef CONFIG_MY_USART_BUS_ID
#define CONFIG_MY_USART_BUS_ID 1
#endif

#ifndef CONFIG_MY_USART_TXD_PIN
#define CONFIG_MY_USART_TXD_PIN 15
#endif

#ifndef CONFIG_MY_USART_RXD_PIN
#define CONFIG_MY_USART_RXD_PIN 16
#endif

#ifndef CONFIG_MY_USART_TXD_PIN_MODE
#define CONFIG_MY_USART_TXD_PIN_MODE 2
#endif

#ifndef CONFIG_MY_USART_RXD_PIN_MODE
#define CONFIG_MY_USART_RXD_PIN_MODE 2
#endif

/* ---------- 全局缓冲区 ---------- */
static uint8_t g_usart_rx_buff[USART_TRANSFER_SIZE] = {0};

static uart_buffer_config_t g_usart_buffer_config = {.rx_buffer = g_usart_rx_buff,
                                                     .rx_buffer_size = USART_TRANSFER_SIZE};

/* 中断模式相关变量 */
#if defined(CONFIG_MY_USART_USE_INT_MODE)
static uint8_t g_usart_int_rx_flag = 0;
static volatile uint16_t g_usart_int_index = 0;
static uint8_t g_usart_int_rx_buff[USART_TRANSFER_SIZE] = {0};
#endif

/**
 * 初始化USART引脚
 */
static void usart_init_pin(void)
{
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(CONFIG_MY_USART_RXD_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(CONFIG_MY_USART_TXD_PIN, CONFIG_MY_USART_TXD_PIN_MODE);
    uapi_pin_set_mode(CONFIG_MY_USART_RXD_PIN, CONFIG_MY_USART_RXD_PIN_MODE);
}

/**
 * 初始化USART配置
 */
static void usart_init_config(void)
{
    uart_attr_t attr = {.baud_rate = USART_BAUDRATE,
                        .data_bits = UART_DATA_BIT_8,
                        .stop_bits = UART_STOP_BIT_1,
                        .parity = UART_PARITY_NONE};

    uart_pin_config_t pin_config = {
        .tx_pin = CONFIG_MY_USART_TXD_PIN, .rx_pin = CONFIG_MY_USART_RXD_PIN, .cts_pin = PIN_NONE, .rts_pin = PIN_NONE};

    uapi_uart_deinit(CONFIG_MY_USART_BUS_ID);
    uapi_uart_init(CONFIG_MY_USART_BUS_ID, &pin_config, &attr, NULL, &g_usart_buffer_config);
}

#if defined(CONFIG_MY_USART_USE_INT_MODE)
/**
 * USART中断接收回调
 */
static void usart_read_int_handler(const void *buffer, uint16_t length, bool error)
{
    unused(error);
    if (buffer == NULL || length == 0) {
        osal_printk("usart%d int mode transfer illegal data!\r\n", CONFIG_MY_USART_BUS_ID);
        return;
    }

    uint8_t *buff = (uint8_t *)buffer;
    osal_printk("usart%d read data: ", CONFIG_MY_USART_BUS_ID);
    for (uint16_t i = 0; i < length; i++) {
        osal_printk("%02X ", buff[i]);
    }
    osal_printk("\r\n");

    if (g_usart_int_index + length > USART_TRANSFER_SIZE) {
        g_usart_int_index = 0;
    }
    if (memcpy_s(g_usart_int_rx_buff + g_usart_int_index, length, buff, length) != EOK) {
        g_usart_int_index = 0;
        osal_printk("usart%d int mode data copy fail!\r\n", CONFIG_MY_USART_BUS_ID);
    }
    g_usart_int_index += length;
    g_usart_int_rx_flag = 1;
}

/**
 * USART中断发送回调
 */
static void usart_write_int_handler(const void *buffer, uint32_t length, const void *params)
{
    unused(params);
    uint8_t *buff = (void *)buffer;
    osal_printk("usart%d write data: ", CONFIG_MY_USART_BUS_ID);
    for (uint16_t i = 0; i < length; i++) {
        osal_printk("%02X ", buff[i]);
    }
    osal_printk("\r\n");
}

/**
 * 注册中断接收回调
 */
static void usart_register_rx_callback(void)
{
    osal_printk("usart%d int mode register receive callback start!\r\n", CONFIG_MY_USART_BUS_ID);
    if (uapi_uart_register_rx_callback(CONFIG_MY_USART_BUS_ID, UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE, 1,
                                       usart_read_int_handler) == ERRCODE_SUCC) {
        osal_printk("usart%d int mode register receive callback succ!\r\n", CONFIG_MY_USART_BUS_ID);
    }
}
#endif

/**
 * USART任务主函数
 */
static void *usart_task(const char *arg)
{
    unused(arg);

    /* 初始化USART引脚 */
    usart_init_pin();

    /* 初始化USART配置 */
    usart_init_config();

    osal_printk("USART Sample Running on BUS %d, TX Pin %d, RX Pin %d...\r\n", CONFIG_MY_USART_BUS_ID,
                CONFIG_MY_USART_TXD_PIN, CONFIG_MY_USART_RXD_PIN);

#if defined(CONFIG_MY_USART_USE_INT_MODE)
    /* 中断模式 */
    usart_register_rx_callback();

    while (1) {
        while (g_usart_int_rx_flag != 1) {
            osal_msleep(USART_INT_WAIT_MS);
        }
        g_usart_int_rx_flag = 0;

        osal_printk("usart%d int mode send back!\r\n", CONFIG_MY_USART_BUS_ID);
        if (uapi_uart_write_int(CONFIG_MY_USART_BUS_ID, g_usart_int_rx_buff, g_usart_int_index, 0,
                                usart_write_int_handler) == ERRCODE_SUCC) {
            osal_printk("usart%d int mode send back succ!\r\n", CONFIG_MY_USART_BUS_ID);
        }
        g_usart_int_index = 0;
    }
#else
    /* 轮询模式 */
    while (1) {
        osal_printk("usart%d poll mode receive start!\r\n", CONFIG_MY_USART_BUS_ID);
        (void)uapi_watchdog_kick();

        int32_t rx_len = uapi_uart_read(CONFIG_MY_USART_BUS_ID, g_usart_rx_buff, USART_TRANSFER_SIZE, 0);
        if (rx_len > 0) {
            osal_printk("usart%d poll mode received %d bytes!\r\n", CONFIG_MY_USART_BUS_ID, rx_len);

            osal_printk("usart%d poll mode send back!\r\n", CONFIG_MY_USART_BUS_ID);
            if (uapi_uart_write(CONFIG_MY_USART_BUS_ID, g_usart_rx_buff, rx_len, 0) == rx_len) {
                osal_printk("usart%d poll mode send back succ!\r\n", CONFIG_MY_USART_BUS_ID);
            }
        }
    }
#endif

    return NULL;
}

/**
 * USART入口函数
 */
static void usart_entry(void)
{
    osal_task *task_handle = NULL;

    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)usart_task, NULL, "usart_task", USART_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, USART_TASK_PRIO);
        osal_printk("USART task created successfully.\r\n");
    } else {
        osal_printk("Create USART task failed.\r\n");
    }
    osal_kthread_unlock();
}

/* 注册应用入口 */
app_run(usart_entry);

#endif /* CONFIG_SAMPLE_SUPPORT_MY_USART */
