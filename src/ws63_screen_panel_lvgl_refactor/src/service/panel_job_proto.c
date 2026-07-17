/**
 * @file panel_job_proto.c
 * @brief Shared sequence allocator for Screen->RX SLE Job packets.
 */
#include "panel_job_proto.h"

#include "soc_osal.h"

static uint16_t g_panel_tx_seq = 1;

uint16_t panel_job_proto_next_seq(void)
{
    uint16_t seq;

    uint32_t lock = osal_irq_lock();
    seq = g_panel_tx_seq++;
    if (g_panel_tx_seq == 0U) {
        g_panel_tx_seq = 1U;
    }
    osal_irq_restore(lock);

    return seq;
}

uint16_t panel_job_proto_peek_seq(void)
{
    uint16_t seq;

    uint32_t lock = osal_irq_lock();
    seq = g_panel_tx_seq;
    osal_irq_restore(lock);

    return seq;
}
