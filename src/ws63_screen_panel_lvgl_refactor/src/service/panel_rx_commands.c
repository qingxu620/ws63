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
#define PANEL_RX_CMD_CONNECT_WAIT_MS 3000U
#define PANEL_RX_CMD_RETRY_MAX       2U
#define PANEL_RX_CMD_IDLE_WAIT_MS    50U

#define PANEL_RX_CMD_STOP       0x01U
#define PANEL_RX_CMD_RESUME     0x02U
#define PANEL_RX_CMD_ABORT      0x04U
#define PANEL_RX_CMD_FOCUS_ON   0x08U
#define PANEL_RX_CMD_FOCUS_OFF  0x10U
#define PANEL_RX_CMD_STATUS     0x20U

static osal_semaphore g_ack_sem;
static osal_semaphore g_cmd_sem;
static volatile bool g_ack_sem_ready;
static volatile bool g_cmd_sem_ready;
static volatile bool g_wait_active;
static volatile bool g_wait_got_ack;
static volatile uint16_t g_wait_ack_seq;
static volatile uint8_t g_wait_status;
static volatile uint32_t g_pending_mask;
static volatile uint8_t g_pending_focus_power;
static volatile bool g_task_started;
static volatile bool g_offline_upload_active;
static volatile bool g_offline_upload_paused;

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

static errcode_t ensure_rx_control_link(void)
{
    if (!panel_transport_sle_can_control_rx()) {
        return ERRCODE_SLE_FAIL;
    }

    panel_transport_sle_set_standalone_session_active(true);
    uint32_t waited_ms = 0;
    while (!panel_transport_sle_rx_is_connected()) {
        if (!panel_transport_sle_can_control_rx()) {
            return ERRCODE_SLE_FAIL;
        }
        if (waited_ms >= PANEL_RX_CMD_CONNECT_WAIT_MS) {
            return ERRCODE_SLE_TIMEOUT;
        }
        panel_transport_sle_poll();
        osal_msleep(20);
        waited_ms += 20U;
    }

    return ERRCODE_SUCC;
}

static errcode_t send_wait_ack(uint8_t type, const void *payload, uint16_t payload_len)
{
    if (ensure_rx_control_link() != ERRCODE_SUCC) {
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
    if (ensure_rx_control_link() != ERRCODE_SUCC) {
        return ERRCODE_SLE_FAIL;
    }
    return encode_and_send(PKT_STATUS_REQ, panel_job_proto_next_seq(), NULL, 0);
}

static errcode_t send_focus(bool on, uint8_t power)
{
    focus_ctrl_payload_t fp = {0};
    fp.on = on ? 1U : 0U;
    fp.power = on ? power : 0U;
    return send_wait_ack(PKT_FOCUS_CTRL, &fp, sizeof(fp));
}

static void release_session_after_result(panel_rx_command_type_t type, errcode_t ret)
{
    if (g_offline_upload_active) {
        return;
    }
    if (ret != ERRCODE_SUCC || type == PANEL_RX_COMMAND_ABORT ||
        type == PANEL_RX_COMMAND_FOCUS_OFF ||
        type == PANEL_RX_COMMAND_STATUS) {
        panel_transport_sle_set_standalone_session_active(false);
    }
}

static uint32_t pop_next_pending(void)
{
    uint32_t lock = osal_irq_lock();
    uint32_t bit = 0;
    if ((g_pending_mask & PANEL_RX_CMD_ABORT) != 0U) {
        bit = PANEL_RX_CMD_ABORT;
        g_pending_mask = 0;
    } else if ((g_pending_mask & PANEL_RX_CMD_STOP) != 0U) {
        bit = PANEL_RX_CMD_STOP;
        g_pending_mask &= ~PANEL_RX_CMD_STOP;
    } else if ((g_pending_mask & PANEL_RX_CMD_RESUME) != 0U) {
        bit = PANEL_RX_CMD_RESUME;
        g_pending_mask &= ~PANEL_RX_CMD_RESUME;
    } else if ((g_pending_mask & PANEL_RX_CMD_FOCUS_OFF) != 0U) {
        bit = PANEL_RX_CMD_FOCUS_OFF;
        g_pending_mask &= ~PANEL_RX_CMD_FOCUS_OFF;
    } else if ((g_pending_mask & PANEL_RX_CMD_FOCUS_ON) != 0U) {
        bit = PANEL_RX_CMD_FOCUS_ON;
        g_pending_mask &= ~PANEL_RX_CMD_FOCUS_ON;
    } else if ((g_pending_mask & PANEL_RX_CMD_STATUS) != 0U) {
        bit = PANEL_RX_CMD_STATUS;
        g_pending_mask &= ~PANEL_RX_CMD_STATUS;
    }
    osal_irq_restore(lock);
    return bit;
}

static errcode_t queue_cmd(uint32_t bit)
{
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        osal_printk("[PANEL_CMD] reject online display-only bit=0x%x\r\n", (unsigned int)bit);
        return ERRCODE_SLE_FAIL;
    }
    if (!panel_transport_sle_can_control_rx()) {
        osal_printk("[PANEL_CMD] reject display-only bit=0x%x\r\n", (unsigned int)bit);
        return ERRCODE_SLE_FAIL;
    }

    uint32_t lock = osal_irq_lock();
    if (bit == PANEL_RX_CMD_ABORT) {
        g_pending_mask = PANEL_RX_CMD_ABORT;
    } else if (bit == PANEL_RX_CMD_STOP) {
        if ((g_pending_mask & PANEL_RX_CMD_ABORT) == 0U) {
            g_pending_mask &= ~PANEL_RX_CMD_RESUME;
            g_pending_mask |= PANEL_RX_CMD_STOP;
        }
    } else if (bit == PANEL_RX_CMD_RESUME) {
        if ((g_pending_mask & PANEL_RX_CMD_ABORT) == 0U) {
            g_pending_mask &= ~PANEL_RX_CMD_STOP;
            g_pending_mask |= PANEL_RX_CMD_RESUME;
        }
    } else {
        if ((g_pending_mask & PANEL_RX_CMD_ABORT) == 0U) {
            g_pending_mask |= bit;
        }
    }
    osal_irq_restore(lock);
    if (g_cmd_sem_ready) {
        osal_sem_up(&g_cmd_sem);
    }
    return ERRCODE_SUCC;
}

static panel_rx_command_result_t dispatch_bit(uint32_t bit)
{
    panel_rx_command_result_t result = {
        .type = PANEL_RX_COMMAND_NONE,
        .ret = ERRCODE_SUCC,
    };

    switch (bit) {
    case PANEL_RX_CMD_STOP:
        result.type = PANEL_RX_COMMAND_EXEC_STOP;
        result.ret = send_wait_ack(PKT_EXEC_STOP, NULL, 0);
        if (result.ret == ERRCODE_SUCC) {
            g_offline_upload_paused = true;
        }
        osal_printk("[PANEL_CMD] EXEC_STOP ret=0x%x\r\n", result.ret);
        break;
    case PANEL_RX_CMD_RESUME:
        result.type = PANEL_RX_COMMAND_EXEC_RESUME;
        result.ret = send_wait_ack(PKT_EXEC_RESUME, NULL, 0);
        if (result.ret == ERRCODE_SUCC) {
            g_offline_upload_paused = false;
        }
        osal_printk("[PANEL_CMD] EXEC_RESUME ret=0x%x\r\n", result.ret);
        break;
    case PANEL_RX_CMD_ABORT:
        result.type = PANEL_RX_COMMAND_ABORT;
        result.ret = send_wait_ack(PKT_JOB_ABORT, NULL, 0);
        if (result.ret == ERRCODE_SUCC) {
            g_offline_upload_paused = false;
        }
        osal_printk("[PANEL_CMD] JOB_ABORT ret=0x%x\r\n", result.ret);
        break;
    case PANEL_RX_CMD_FOCUS_OFF:
        result.type = PANEL_RX_COMMAND_FOCUS_OFF;
        result.ret = send_focus(false, 0);
        if (result.ret == ERRCODE_SUCC) {
            panel_model_mark_focus_ack(false);
        }
        osal_printk("[PANEL_CMD] FOCUS_OFF ret=0x%x\r\n", result.ret);
        break;
    case PANEL_RX_CMD_FOCUS_ON:
        result.type = PANEL_RX_COMMAND_FOCUS_ON;
        {
            uint8_t power = g_pending_focus_power;
            if (power > 100U) {
                power = 100U;
            }
            result.ret = send_focus(true, power);
            if (result.ret == ERRCODE_SUCC) {
                panel_model_mark_focus_ack(true);
            }
            osal_printk("[PANEL_CMD] FOCUS_ON s=%u ret=0x%x\r\n",
                        (unsigned int)power, result.ret);
        }
        break;
    case PANEL_RX_CMD_STATUS:
        result.type = PANEL_RX_COMMAND_STATUS;
        result.ret = send_status_req();
        osal_printk("[PANEL_CMD] STATUS_REQ ret=0x%x\r\n", result.ret);
        break;
    default:
        break;
    }

    release_session_after_result(result.type, result.ret);
    return result;
}

static int panel_rx_cmd_task(void *arg)
{
    unused(arg);

    while (1) {
        if (g_offline_upload_active) {
            if (g_cmd_sem_ready) {
                (void)osal_sem_down_timeout(&g_cmd_sem, PANEL_RX_CMD_IDLE_WAIT_MS);
            } else {
                osal_msleep(PANEL_RX_CMD_IDLE_WAIT_MS);
            }
            continue;
        }
        uint32_t bit = pop_next_pending();
        if (bit == 0U) {
            if (g_cmd_sem_ready) {
                (void)osal_sem_down_timeout(&g_cmd_sem, PANEL_RX_CMD_IDLE_WAIT_MS);
            } else {
                osal_msleep(PANEL_RX_CMD_IDLE_WAIT_MS);
            }
            continue;
        }
        (void)dispatch_bit(bit);
    }
    return 0;
}

errcode_t panel_rx_commands_init(void)
{
    if (!g_ack_sem_ready && osal_sem_init(&g_ack_sem, 0) != OSAL_SUCCESS) {
        return ERRCODE_FAIL;
    }
    g_ack_sem_ready = true;
    if (!g_cmd_sem_ready && osal_sem_init(&g_cmd_sem, 0) != OSAL_SUCCESS) {
        return ERRCODE_FAIL;
    }
    g_cmd_sem_ready = true;
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

errcode_t panel_rx_commands_request_exec_resume(void)
{
    return queue_cmd(PANEL_RX_CMD_RESUME);
}

errcode_t panel_rx_commands_request_abort(void)
{
    return queue_cmd(PANEL_RX_CMD_ABORT);
}

errcode_t panel_rx_commands_request_focus_on(uint8_t power)
{
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        osal_printk("[PANEL_CMD] reject online display-only focus_on\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (!panel_transport_sle_can_control_rx()) {
        osal_printk("[PANEL_CMD] reject display-only focus_on\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (power > 100U) {
        power = 100U;
    }
    uint32_t lock = osal_irq_lock();
    if ((g_pending_mask & PANEL_RX_CMD_ABORT) == 0U) {
        g_pending_focus_power = power;
        g_pending_mask |= PANEL_RX_CMD_FOCUS_ON;
    }
    osal_irq_restore(lock);
    if (g_cmd_sem_ready) {
        osal_sem_up(&g_cmd_sem);
    }
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

void panel_rx_commands_set_offline_upload_active(bool active)
{
    g_offline_upload_active = active;
    if (!active) {
        g_offline_upload_paused = false;
    }
    if (g_cmd_sem_ready) {
        osal_sem_up(&g_cmd_sem);
    }
}

bool panel_rx_commands_is_offline_upload_paused(void)
{
    return g_offline_upload_paused;
}

bool panel_rx_commands_has_pending(void)
{
    return g_pending_mask != 0U;
}

panel_rx_command_result_t panel_rx_commands_dispatch_pending(void)
{
    uint32_t bit = pop_next_pending();
    if (bit == 0U) {
        panel_rx_command_result_t result = {
            .type = PANEL_RX_COMMAND_NONE,
            .ret = ERRCODE_SUCC,
        };
        return result;
    }
    return dispatch_bit(bit);
}
