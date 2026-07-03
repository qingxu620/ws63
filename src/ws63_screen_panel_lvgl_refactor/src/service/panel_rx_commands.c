/**
 * @file panel_rx_commands.c
 * @brief Host-aligned Screen->RX control command sender.
 */
#include "panel_rx_commands.h"

#include "panel_job_proto.h"
#include "panel_model.h"
#include "panel_transport_sle.h"
#include "task_manager.h"

#include "common_def.h"
#include "packet.h"
#include "protocol.h"
#include "securec.h"
#include "sle_errcode.h"
#include "soc_osal.h"

#include <stdbool.h>
#include <string.h>

#define PANEL_RX_CMD_TASK_STACK_SIZE 0x1400
#define PANEL_RX_CMD_TASK_PRIORITY   6
#define PANEL_RX_CMD_ACK_TIMEOUT_MS  1000U
#define PANEL_RX_CMD_RETRY_MAX       2U

#define PANEL_RX_CMD_STOP       0x01U
#define PANEL_RX_CMD_ABORT      0x02U
#define PANEL_RX_CMD_FOCUS_ON   0x04U
#define PANEL_RX_CMD_FOCUS_OFF  0x08U
#define PANEL_RX_CMD_STATUS     0x10U

static osal_semaphore g_ack_sem;
static volatile bool g_ack_sem_ready;
static volatile bool g_wait_active;
static volatile bool g_wait_got_ack;
static volatile uint16_t g_wait_ack_seq;
static volatile uint8_t g_wait_status;
static volatile uint32_t g_pending_mask;
static volatile uint8_t g_pending_focus_power;
static volatile bool g_task_started;

static void cmd_response_cb(const uint8_t *data, uint16_t len)
{
    sle_packet_view_t pkt;
    if (data == NULL || len == 0 || !sle_packet_decode(data, len, &pkt)) {
        return;
    }

    if ((pkt.type != PKT_ACK && pkt.type != PKT_NACK) ||
        pkt.len != sizeof(ack_payload_t)) {
        return;
    }

    ack_payload_t ack;
    (void)memcpy_s(&ack, sizeof(ack), pkt.payload, sizeof(ack));
    if (!g_wait_active || ack.ack_seq != g_wait_ack_seq) {
        return;
    }

    g_wait_status = ack.status;
    g_wait_got_ack = true;
    if (g_ack_sem_ready) {
        osal_sem_up(&g_ack_sem);
    }
}

static errcode_t encode_and_send(uint8_t type, uint16_t seq,
                                 const void *payload, uint16_t payload_len)
{
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;

    if (!sle_packet_encode(type, 0, seq, payload, payload_len,
                           packet, sizeof(packet), &packet_len)) {
        return ERRCODE_FAIL;
    }
    return panel_transport_sle_send_rx_packet(packet, packet_len);
}

static errcode_t send_wait_ack(uint8_t type, const void *payload, uint16_t payload_len)
{
    if (!panel_transport_sle_rx_is_connected()) {
        osal_printk("[PANEL_CMD] no RX link type=0x%02x\r\n", type);
        return ERRCODE_SLE_FAIL;
    }

    uint16_t seq = 0;
    g_wait_active = true;

    for (uint32_t retry = 0; retry <= PANEL_RX_CMD_RETRY_MAX; retry++) {
        g_wait_ack_seq = 0;
        while (g_ack_sem_ready && osal_sem_down_timeout(&g_ack_sem, 0) == OSAL_SUCCESS) {
        }
        g_wait_status = JOB_STATUS_INTERNAL_ERROR;
        g_wait_got_ack = false;
        seq = panel_job_proto_next_seq();
        g_wait_ack_seq = seq;
        errcode_t ret = encode_and_send(type, seq, payload, payload_len);
        if (ret == ERRCODE_SLE_SUCCESS &&
            osal_sem_down_timeout(&g_ack_sem, PANEL_RX_CMD_ACK_TIMEOUT_MS) == OSAL_SUCCESS &&
            g_wait_got_ack) {
            g_wait_active = false;
            g_wait_ack_seq = 0;
            return (g_wait_status == JOB_STATUS_OK) ? ERRCODE_SUCC : ERRCODE_FAIL;
        }
        osal_printk("[PANEL_CMD] wait ack retry type=0x%02x seq=%u try=%u st=%u\r\n",
                    type, (unsigned int)seq, (unsigned int)retry,
                    (unsigned int)g_wait_status);
    }

    g_wait_active = false;
    g_wait_ack_seq = 0;
    return ERRCODE_SLE_TIMEOUT;
}

static errcode_t send_status_req(void)
{
    return encode_and_send(PKT_STATUS_REQ, panel_job_proto_next_seq(), NULL, 0);
}

static errcode_t send_focus(bool on, uint8_t power)
{
    focus_ctrl_payload_t fp = {0};
    fp.on = on ? 1U : 0U;
    fp.power = on ? power : 0U;
    return send_wait_ack(PKT_FOCUS_CTRL, &fp, sizeof(fp));
}

static uint32_t take_pending_mask(void)
{
    uint32_t lock = osal_irq_lock();
    uint32_t mask = g_pending_mask;
    g_pending_mask = 0;
    osal_irq_restore(lock);
    return mask;
}

static errcode_t queue_cmd(uint32_t bit)
{
    uint32_t lock = osal_irq_lock();
    g_pending_mask |= bit;
    osal_irq_restore(lock);
    return ERRCODE_SUCC;
}

static int panel_rx_cmd_task(void *arg)
{
    unused(arg);

    while (1) {
        uint32_t mask = take_pending_mask();
        if (mask == 0U) {
            osal_msleep(20);
            continue;
        }

        if ((mask & PANEL_RX_CMD_STOP) != 0U) {
            errcode_t ret = send_wait_ack(PKT_EXEC_STOP, NULL, 0);
            osal_printk("[PANEL_CMD] EXEC_STOP ret=0x%x\r\n", ret);
        }
        if ((mask & PANEL_RX_CMD_ABORT) != 0U) {
            errcode_t ret = send_wait_ack(PKT_JOB_ABORT, NULL, 0);
            osal_printk("[PANEL_CMD] JOB_ABORT ret=0x%x\r\n", ret);
        }
        if ((mask & PANEL_RX_CMD_FOCUS_OFF) != 0U) {
            errcode_t ret = send_focus(false, 0);
            if (ret == ERRCODE_SUCC) {
                panel_model_mark_focus_ack(false);
            }
            osal_printk("[PANEL_CMD] FOCUS_OFF ret=0x%x\r\n", ret);
        }
        if ((mask & PANEL_RX_CMD_FOCUS_ON) != 0U) {
            uint8_t power = g_pending_focus_power;
            if (power > 100U) {
                power = 100U;
            }
            errcode_t ret = send_focus(true, power);
            if (ret == ERRCODE_SUCC) {
                panel_model_mark_focus_ack(true);
            }
            osal_printk("[PANEL_CMD] FOCUS_ON s=%u ret=0x%x\r\n",
                        (unsigned int)power, ret);
        }
        if ((mask & PANEL_RX_CMD_STATUS) != 0U) {
            errcode_t ret = send_status_req();
            osal_printk("[PANEL_CMD] STATUS_REQ ret=0x%x\r\n", ret);
        }
    }
    return 0;
}

errcode_t panel_rx_commands_init(void)
{
    if (!g_ack_sem_ready && osal_sem_init(&g_ack_sem, 0) != OSAL_SUCCESS) {
        return ERRCODE_FAIL;
    }
    g_ack_sem_ready = true;
    panel_transport_sle_set_cmd_response_cb(cmd_response_cb);

    if (g_task_started) {
        return ERRCODE_SUCC;
    }
    errcode_t ret = task_create("panel_rx_cmd", panel_rx_cmd_task, NULL,
                                PANEL_RX_CMD_TASK_STACK_SIZE,
                                PANEL_RX_CMD_TASK_PRIORITY);
    if (ret == ERRCODE_SUCC) {
        g_task_started = true;
    }
    return ret;
}

errcode_t panel_rx_commands_request_exec_stop(void)
{
    return queue_cmd(PANEL_RX_CMD_STOP);
}

errcode_t panel_rx_commands_request_abort(void)
{
    return queue_cmd(PANEL_RX_CMD_ABORT);
}

errcode_t panel_rx_commands_request_focus_on(uint8_t power)
{
    if (power > 100U) {
        power = 100U;
    }
    uint32_t lock = osal_irq_lock();
    g_pending_focus_power = power;
    g_pending_mask |= PANEL_RX_CMD_FOCUS_ON;
    osal_irq_restore(lock);
    return ERRCODE_SUCC;
}

errcode_t panel_rx_commands_request_focus_off(void)
{
    return queue_cmd(PANEL_RX_CMD_FOCUS_OFF);
}

errcode_t panel_rx_commands_request_status(void)
{
    return queue_cmd(PANEL_RX_CMD_STATUS);
}
