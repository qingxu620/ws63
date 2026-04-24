/**
 * @file zdt_controller.c
 * @brief ZDT 高层控制接口
 */
#include "zdt_controller.h"
#include "config.h"

static zdt_controller_config_t g_zdt_controller_config;

static errcode_t zdt_controller_transact_ack(
    const uint8_t *tx_frame, uint16_t tx_len, uint8_t cmd, uint8_t addr, uint8_t *status)
{
    uint8_t rx_frame[4] = {0};
    errcode_t ret;

    ret = zdt_uart_transact(tx_frame, tx_len, rx_frame, sizeof(rx_frame), g_zdt_controller_config.reply_timeout_ms);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return zdt_parse_simple_ack(rx_frame, sizeof(rx_frame), addr, cmd, status);
}

static errcode_t zdt_controller_transact_read_u8(
    const uint8_t *tx_frame, uint16_t tx_len, uint8_t cmd, uint8_t *value)
{
    uint8_t rx_frame[4] = {0};
    errcode_t ret;

    ret = zdt_uart_transact(
        tx_frame, tx_len, rx_frame, sizeof(rx_frame), g_zdt_controller_config.reply_timeout_ms + 50U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return zdt_parse_u8_reply(rx_frame, sizeof(rx_frame), g_zdt_controller_config.device_addr, cmd, value);
}

static errcode_t zdt_controller_transact_read_i16(
    const uint8_t *tx_frame, uint16_t tx_len, uint8_t cmd, int16_t *value)
{
    uint8_t rx_frame[6] = {0};
    errcode_t ret;

    ret = zdt_uart_transact(
        tx_frame, tx_len, rx_frame, sizeof(rx_frame), g_zdt_controller_config.reply_timeout_ms + 50U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return zdt_parse_signed16_reply(rx_frame, sizeof(rx_frame), g_zdt_controller_config.device_addr, cmd, value);
}

static errcode_t zdt_controller_transact_read_i32(
    const uint8_t *tx_frame, uint16_t tx_len, uint8_t cmd, int32_t *value)
{
    uint8_t rx_frame[8] = {0};
    errcode_t ret;

    ret = zdt_uart_transact(
        tx_frame, tx_len, rx_frame, sizeof(rx_frame), g_zdt_controller_config.reply_timeout_ms + 50U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return zdt_parse_signed32_reply(rx_frame, sizeof(rx_frame), g_zdt_controller_config.device_addr, cmd, value);
}

errcode_t zdt_controller_init(const zdt_uart_config_t *uart_config, const zdt_controller_config_t *controller_config)
{
    if (uart_config == NULL || controller_config == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    g_zdt_controller_config = *controller_config;
    if (g_zdt_controller_config.reply_timeout_ms == 0U) {
        g_zdt_controller_config.reply_timeout_ms = 200U;
    }

    return zdt_uart_init(uart_config);
}

errcode_t zdt_controller_enable_motor(bool enable, bool sync, uint8_t *status)
{
    uint8_t frame[6] = {0};
    size_t frame_len = zdt_build_enable_frame(g_zdt_controller_config.device_addr, enable, sync, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0xF3, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_run_speed(
    zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, bool sync, uint8_t *status)
{
    uint8_t frame[8] = {0};
    size_t frame_len = zdt_build_speed_frame(
        g_zdt_controller_config.device_addr, direction, speed_rpm, accel_level, sync, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0xF6, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_run_position(
    zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, uint32_t pulses,
    zdt_position_mode_t position_mode, bool sync, uint8_t *status)
{
    uint8_t frame[13] = {0};
    size_t frame_len = zdt_build_position_frame(
        g_zdt_controller_config.device_addr, direction, speed_rpm, accel_level, pulses, position_mode, sync, frame,
        sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0xFD, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_stop_now(bool sync, uint8_t *status)
{
    uint8_t frame[5] = {0};
    size_t frame_len = zdt_build_stop_now_frame(g_zdt_controller_config.device_addr, sync, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0xFE, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_sync_start(uint8_t target_addr, uint8_t *status)
{
    uint8_t frame[4] = {0};
    size_t frame_len = zdt_build_sync_start_frame(target_addr, frame, sizeof(frame));
    errcode_t ret;

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }

    if (target_addr == 0U) {
        ret = zdt_uart_write_frame(frame, (uint16_t)frame_len);
        if (ret == ERRCODE_SUCC && status != NULL) {
            *status = ZDT_PROTOCOL_STATUS_OK;
        }
        return ret;
    }

    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0xFF, target_addr, status);
}

errcode_t zdt_controller_set_single_turn_zero(bool persist, uint8_t *status)
{
    uint8_t frame[5] = {0};
    size_t frame_len =
        zdt_build_set_single_turn_zero_frame(g_zdt_controller_config.device_addr, persist, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0x93, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_clear_position(uint8_t *status)
{
    uint8_t frame[4] = {0};
    size_t frame_len = zdt_build_clear_position_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0x0A, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_release_stall(uint8_t *status)
{
    uint8_t frame[4] = {0};
    size_t frame_len = zdt_build_release_stall_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0x0E, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_modify_control_mode(bool persist, zdt_control_mode_t control_mode, uint8_t *status)
{
    uint8_t frame[6] = {0};
    size_t frame_len = zdt_build_modify_control_mode_frame(
        g_zdt_controller_config.device_addr, persist, control_mode, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0x46, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_trigger_home(zdt_home_mode_t mode, bool sync, uint8_t *status)
{
    uint8_t frame[5] = {0};
    size_t frame_len =
        zdt_build_trigger_home_frame(g_zdt_controller_config.device_addr, mode, sync, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0x9A, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_modify_home_params(bool persist, const zdt_home_params_t *home_params, uint8_t *status)
{
    uint8_t frame[20] = {0};
    size_t frame_len = zdt_build_modify_home_params_frame(
        g_zdt_controller_config.device_addr, persist, home_params, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0x4C, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_abort_home(uint8_t *status)
{
    uint8_t frame[4] = {0};
    size_t frame_len = zdt_build_abort_home_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_ack(frame, (uint16_t)frame_len, 0x9C, g_zdt_controller_config.device_addr, status);
}

errcode_t zdt_controller_read_sys_params_raw(zdt_sys_param_t param, uint8_t *frame, uint16_t frame_size, uint16_t *frame_len)
{
    uint8_t tx_frame[4] = {0};
    size_t tx_len =
        zdt_build_read_sys_params_frame(g_zdt_controller_config.device_addr, param, tx_frame, sizeof(tx_frame));

    if (frame == NULL || frame_size == 0U || frame_len == NULL || tx_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }

    return zdt_uart_transact_frame(
        tx_frame, (uint16_t)tx_len, frame, frame_size, frame_len, g_zdt_controller_config.reply_timeout_ms + 50U,
        ZDT_UART_IDLE_GAP_MS);
}

errcode_t zdt_controller_read_version(zdt_version_info_t *version_info)
{
    uint8_t frame[3] = {0};
    uint8_t rx_frame[5] = {0};
    size_t frame_len = zdt_build_read_version_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));
    errcode_t ret;

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }

    ret = zdt_uart_transact(
        frame, (uint16_t)frame_len, rx_frame, sizeof(rx_frame), g_zdt_controller_config.reply_timeout_ms + 50U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return zdt_parse_version_reply(rx_frame, sizeof(rx_frame), g_zdt_controller_config.device_addr, version_info);
}

errcode_t zdt_controller_read_motor_status(uint8_t *flags)
{
    uint8_t frame[3] = {0};
    size_t frame_len = zdt_build_read_status_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_read_u8(frame, (uint16_t)frame_len, 0x3A, flags);
}

errcode_t zdt_controller_read_home_status(uint8_t *flags)
{
    uint8_t frame[3] = {0};
    size_t frame_len = zdt_build_read_home_status_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_read_u8(frame, (uint16_t)frame_len, 0x3B, flags);
}

errcode_t zdt_controller_read_real_speed(int16_t *speed_rpm)
{
    uint8_t frame[3] = {0};
    size_t frame_len = zdt_build_read_real_speed_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_read_i16(frame, (uint16_t)frame_len, 0x35, speed_rpm);
}

errcode_t zdt_controller_read_real_position(int32_t *position_raw)
{
    uint8_t frame[3] = {0};
    size_t frame_len = zdt_build_read_real_position_frame(g_zdt_controller_config.device_addr, frame, sizeof(frame));

    if (frame_len == 0U) {
        return ERRCODE_INVALID_PARAM;
    }
    return zdt_controller_transact_read_i32(frame, (uint16_t)frame_len, 0x36, position_raw);
}

uint32_t zdt_controller_get_reply_timeout_ms(void)
{
    if (g_zdt_controller_config.reply_timeout_ms == 0U) {
        return ZDT_UART_REPLY_TIMEOUT_MS;
    }
    return g_zdt_controller_config.reply_timeout_ms;
}

double zdt_controller_position_raw_to_degrees(int32_t position_raw)
{
    return ((double)position_raw * 360.0) / 65536.0;
}
