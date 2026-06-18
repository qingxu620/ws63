/**
 * @file Emm_V5.c
 * @brief 张大头官方 STM32 例程接口在 WS63 上的兼容封装
 */
#include "Emm_V5.h"

#include <string.h>

#include "zdt_controller.h"
#include "zdt_protocol.h"
#include "zdt_uart.h"

volatile bool rxFrameFlag = false;
volatile uint8_t rxCmd[EMM_V5_RX_FRAME_CAPACITY] = {0};
volatile uint8_t rxCount = 0;

static errcode_t g_emm_v5_last_result = ERRCODE_SUCC;

static zdt_sys_param_t emm_v5_map_sys_param(SysParams_t param)
{
    return (zdt_sys_param_t)param;
}

static void emm_v5_clear_rx_frame_impl(void)
{
    (void)memset((void *)rxCmd, 0, sizeof(rxCmd));
    rxCount = 0;
    rxFrameFlag = false;
}

static void emm_v5_store_rx_frame(const uint8_t *frame, uint16_t frame_len)
{
    uint16_t copy_len;

    emm_v5_clear_rx_frame_impl();
    if (frame == NULL || frame_len == 0U) {
        return;
    }

    copy_len = frame_len;
    if (copy_len > (uint16_t)sizeof(rxCmd)) {
        copy_len = (uint16_t)sizeof(rxCmd);
    }
    (void)memcpy((void *)rxCmd, frame, copy_len);
    if (copy_len > 0xFFU) {
        rxCount = 0xFFU;
    } else {
        rxCount = (uint8_t)copy_len;
    }
    rxFrameFlag = true;
}

static errcode_t emm_v5_validate_raw_reply(const uint8_t *frame, uint16_t frame_len, uint8_t addr, uint8_t cmd)
{
    if (frame == NULL || frame_len < 4U) {
        return ZDT_ERR_REPLY_INVALID;
    }
    if (frame[1] == 0x00U && frame_len >= 4U && frame[2] == ZDT_PROTOCOL_STATUS_BAD_COMMAND) {
        return ZDT_ERR_REMOTE_ERROR;
    }
    if (frame[0] != addr || frame[1] != cmd || frame[frame_len - 1U] != ZDT_PROTOCOL_CHECKSUM_FIXED) {
        return ZDT_ERR_REPLY_INVALID;
    }
    return ERRCODE_SUCC;
}

static void emm_v5_finish_without_reply(errcode_t ret)
{
    g_emm_v5_last_result = ret;
    if (ret != ERRCODE_SUCC) {
        emm_v5_clear_rx_frame_impl();
    }
}

static void emm_v5_transact_ack(uint8_t addr, const uint8_t *tx_frame, uint16_t tx_len, uint8_t expected_cmd)
{
    uint8_t rx_frame[4] = {0};
    uint8_t ack_status = 0;
    errcode_t ret;

    if (tx_frame == NULL || tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }

    ret = zdt_uart_transact(tx_frame, tx_len, rx_frame, sizeof(rx_frame), zdt_controller_get_reply_timeout_ms());
    if (ret == ERRCODE_SUCC) {
        ret = zdt_parse_simple_ack(rx_frame, sizeof(rx_frame), addr, expected_cmd, &ack_status);
    }
    if (ret == ERRCODE_SUCC || ret == ZDT_ERR_REMOTE_REJECTED || ret == ZDT_ERR_REMOTE_ERROR) {
        emm_v5_store_rx_frame(rx_frame, sizeof(rx_frame));
    } else {
        emm_v5_clear_rx_frame_impl();
    }
    g_emm_v5_last_result = ret;
}

static void emm_v5_transact_read(uint8_t addr, const uint8_t *tx_frame, uint16_t tx_len, uint8_t expected_cmd)
{
    uint8_t rx_frame[EMM_V5_RX_FRAME_CAPACITY] = {0};
    uint16_t rx_len = 0;
    errcode_t ret;

    if (tx_frame == NULL || tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }

    ret = zdt_uart_transact_frame(
        tx_frame, tx_len, rx_frame, sizeof(rx_frame), &rx_len, zdt_controller_get_reply_timeout_ms() + 50U,
        ZDT_UART_IDLE_GAP_MS);
    if (ret == ERRCODE_SUCC) {
        ret = emm_v5_validate_raw_reply(rx_frame, rx_len, addr, expected_cmd);
    }

    if (ret == ERRCODE_SUCC) {
        emm_v5_store_rx_frame(rx_frame, rx_len);
    } else {
        emm_v5_clear_rx_frame_impl();
    }
    g_emm_v5_last_result = ret;
}

errcode_t Emm_V5_Get_Last_Result(void)
{
    return g_emm_v5_last_result;
}

void Emm_V5_Clear_Rx_Frame(void)
{
    emm_v5_clear_rx_frame_impl();
    g_emm_v5_last_result = ERRCODE_SUCC;
}

void Emm_V5_Reset_CurPos_To_Zero(uint8_t addr)
{
    uint8_t tx_frame[4] = {0};
    size_t tx_len = zdt_build_clear_position_frame(addr, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0x0A);
}

void Emm_V5_Reset_Clog_Pro(uint8_t addr)
{
    uint8_t tx_frame[4] = {0};
    size_t tx_len = zdt_build_release_stall_frame(addr, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0x0E);
}

void Emm_V5_Read_Sys_Params(uint8_t addr, SysParams_t s)
{
    uint8_t tx_frame[4] = {0};
    size_t tx_len = zdt_build_read_sys_params_frame(addr, emm_v5_map_sys_param(s), tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_read(addr, tx_frame, (uint16_t)tx_len, tx_frame[1]);
}

void Emm_V5_Modify_Ctrl_Mode(uint8_t addr, bool svF, uint8_t ctrl_mode)
{
    uint8_t tx_frame[6] = {0};
    size_t tx_len = zdt_build_modify_control_mode_frame(
        addr, svF, (zdt_control_mode_t)ctrl_mode, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0x46);
}

void Emm_V5_En_Control(uint8_t addr, bool state, bool snF)
{
    uint8_t tx_frame[6] = {0};
    size_t tx_len = zdt_build_enable_frame(addr, state, snF, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0xF3);
}

void Emm_V5_Vel_Control(uint8_t addr, uint8_t dir, uint16_t vel, uint8_t acc, bool snF)
{
    uint8_t tx_frame[8] = {0};
    size_t tx_len = zdt_build_speed_frame(addr, (zdt_direction_t)dir, vel, acc, snF, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0xF6);
}

void Emm_V5_Pos_Control(uint8_t addr, uint8_t dir, uint16_t vel, uint8_t acc, uint32_t clk, bool raF, bool snF)
{
    uint8_t tx_frame[13] = {0};
    size_t tx_len = zdt_build_position_frame(
        addr, (zdt_direction_t)dir, vel, acc, clk, raF ? ZDT_POSITION_ABSOLUTE : ZDT_POSITION_RELATIVE, snF, tx_frame,
        sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0xFD);
}

void Emm_V5_Stop_Now(uint8_t addr, bool snF)
{
    uint8_t tx_frame[5] = {0};
    size_t tx_len = zdt_build_stop_now_frame(addr, snF, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0xFE);
}

void Emm_V5_Synchronous_motion(uint8_t addr)
{
    uint8_t tx_frame[4] = {0};
    size_t tx_len = zdt_build_sync_start_frame(addr, tx_frame, sizeof(tx_frame));
    errcode_t ret;

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }

    if (addr == 0U) {
        emm_v5_clear_rx_frame_impl();
        ret = zdt_uart_write_frame(tx_frame, (uint16_t)tx_len);
        emm_v5_finish_without_reply(ret);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0xFF);
}

void Emm_V5_Origin_Set_O(uint8_t addr, bool svF)
{
    uint8_t tx_frame[5] = {0};
    size_t tx_len = zdt_build_set_single_turn_zero_frame(addr, svF, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0x93);
}

void Emm_V5_Origin_Modify_Params(
    uint8_t addr, bool svF, uint8_t o_mode, uint8_t o_dir, uint16_t o_vel, uint32_t o_tm, uint16_t sl_vel,
    uint16_t sl_ma, uint16_t sl_ms, bool potF)
{
    uint8_t tx_frame[20] = {0};
    zdt_home_params_t params = {
        .home_mode = (zdt_home_mode_t)o_mode,
        .direction = (zdt_direction_t)o_dir,
        .home_speed_rpm = o_vel,
        .timeout_ms = o_tm,
        .stall_speed_rpm = sl_vel,
        .stall_current_ma = sl_ma,
        .stall_time_ms = sl_ms,
        .power_on_trigger = potF,
    };
    size_t tx_len = zdt_build_modify_home_params_frame(addr, svF, &params, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0x4C);
}

void Emm_V5_Origin_Trigger_Return(uint8_t addr, uint8_t o_mode, bool snF)
{
    uint8_t tx_frame[5] = {0};
    size_t tx_len = zdt_build_trigger_home_frame(addr, (zdt_home_mode_t)o_mode, snF, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0x9A);
}

void Emm_V5_Origin_Interrupt(uint8_t addr)
{
    uint8_t tx_frame[4] = {0};
    size_t tx_len = zdt_build_abort_home_frame(addr, tx_frame, sizeof(tx_frame));

    if (tx_len == 0U) {
        emm_v5_finish_without_reply(ERRCODE_INVALID_PARAM);
        return;
    }
    emm_v5_transact_ack(addr, tx_frame, (uint16_t)tx_len, 0x9C);
}
