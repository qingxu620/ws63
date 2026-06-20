/**
 * @file rx_core.c
 * @brief Unified RX core skeleton.
 */
#include "rx_core.h"
#include "rx_mode.h"
#include "rx_stream.h"
#include "rx_status.h"
#include "soc_osal.h"

void rx_core_init(void)
{
    rx_mode_init();
    rx_status_init();
    rx_stream_init();
    osal_printk("[RX_CORE] init phase=2A mode=%s\r\n", rx_mode_name(rx_mode_get()));
}

void rx_core_on_stream_ready(rx_stream_src_t src)
{
    rx_stream_on_ready(src);
}

void rx_core_on_stream_poll(rx_stream_src_t src)
{
    rx_stream_on_poll(src);
}

void rx_core_on_stream_byte(rx_stream_src_t src, uint8_t byte)
{
    rx_stream_on_byte(src, byte);
}
