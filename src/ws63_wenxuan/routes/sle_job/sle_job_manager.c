/**
 * @file sle_job_manager.c
 * @brief Wenxuan phone integration overlay for the unified RX job manager.
 *
 * Keep the active unified RX manager as the implementation source so safety,
 * cache, execution, and protocol fixes remain shared.  The phone client also
 * writes the two-byte SSAP CCCD subscription value through the receive path;
 * filter that non-job write before the packet decoder can NACK it or touch
 * sequence state.
 */
#define sle_job_manager_on_packet wenxuan_sle_job_manager_on_packet_base
#include "../../../ws63_laser_rx_unified/routes/sle_job/sle_job_manager.c"
#undef sle_job_manager_on_packet

void sle_job_manager_on_packet(uint16_t conn_id, const uint8_t *data, uint16_t len)
{
    if (len < SLE_JOB_PACKET_HEADER_LEN) {
        osal_printk("[JOB_RX] ignore short non-job write len=%u\r\n", (unsigned int)len);
        return;
    }
    wenxuan_sle_job_manager_on_packet_base(conn_id, data, len);
}
