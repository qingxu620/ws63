/**
 * @file zdt_protocol.c
 * @brief 张大头 Emm_V5.0 自定义串口协议封装
 */
#include "zdt_protocol.h"

enum {
    ZDT_CMD_READ_RL = 0x20,
    ZDT_CMD_READ_PID = 0x21,
    ZDT_CMD_READ_VBUS = 0x24,
    ZDT_CMD_READ_CPHA = 0x27,
    ZDT_CMD_READ_ENCL = 0x31,
    ZDT_CMD_READ_TARGET_POSITION = 0x33,
    ZDT_CMD_ENABLE = 0xF3,
    ZDT_CMD_SPEED = 0xF6,
    ZDT_CMD_POSITION = 0xFD,
    ZDT_CMD_STOP_NOW = 0xFE,
    ZDT_CMD_SYNC_START = 0xFF,
    ZDT_CMD_SET_SINGLE_TURN_ZERO = 0x93,
    ZDT_CMD_CLEAR_POSITION = 0x0A,
    ZDT_CMD_RELEASE_STALL = 0x0E,
    ZDT_CMD_TRIGGER_HOME = 0x9A,
    ZDT_CMD_ABORT_HOME = 0x9C,
    ZDT_CMD_READ_VERSION = 0x1F,
    ZDT_CMD_READ_CONFIG = 0x42,
    ZDT_CMD_READ_SYSTEM_STATE = 0x43,
    ZDT_CMD_MODIFY_CONTROL_MODE = 0x46,
    ZDT_CMD_MODIFY_HOME_PARAMS = 0x4C,
    ZDT_CMD_READ_STATUS = 0x3A,
    ZDT_CMD_READ_HOME_STATUS = 0x3B,
    ZDT_CMD_READ_REAL_SPEED = 0x35,
    ZDT_CMD_READ_REAL_POSITION = 0x36,
    ZDT_CMD_READ_POSITION_ERROR = 0x37,
};

static size_t zdt_finalize_frame(uint8_t *frame, size_t frame_size, size_t payload_len)
{
    if (frame == NULL || frame_size < (payload_len + 1U)) {
        return 0;
    }
    frame[payload_len] = ZDT_PROTOCOL_CHECKSUM_FIXED;
    return payload_len + 1U;
}

static errcode_t zdt_validate_header(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, size_t expect_len)
{
    if (frame == NULL || frame_len != expect_len) {
        return ZDT_ERR_REPLY_INVALID;
    }
    if (frame[0] != addr || frame[frame_len - 1U] != ZDT_PROTOCOL_CHECKSUM_FIXED) {
        return ZDT_ERR_REPLY_INVALID;
    }
    if (frame[1] == 0x00U && frame[2] == ZDT_PROTOCOL_STATUS_BAD_COMMAND) {
        return ZDT_ERR_REMOTE_ERROR;
    }
    if (frame[1] != cmd) {
        return ZDT_ERR_REPLY_INVALID;
    }
    return ERRCODE_SUCC;
}

static errcode_t zdt_get_sys_param_command(zdt_sys_param_t param, uint8_t *cmd, uint8_t *aux, bool *has_aux)
{
    if (cmd == NULL || aux == NULL || has_aux == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    *aux = 0x00U;
    *has_aux = false;

    switch (param) {
        case ZDT_SYS_PARAM_VER:
            *cmd = ZDT_CMD_READ_VERSION;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_RL:
            *cmd = ZDT_CMD_READ_RL;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_PID:
            *cmd = ZDT_CMD_READ_PID;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_VBUS:
            *cmd = ZDT_CMD_READ_VBUS;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_CPHA:
            *cmd = ZDT_CMD_READ_CPHA;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_ENCL:
            *cmd = ZDT_CMD_READ_ENCL;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_TPOS:
            *cmd = ZDT_CMD_READ_TARGET_POSITION;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_VEL:
            *cmd = ZDT_CMD_READ_REAL_SPEED;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_CPOS:
            *cmd = ZDT_CMD_READ_REAL_POSITION;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_PERR:
            *cmd = ZDT_CMD_READ_POSITION_ERROR;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_FLAG:
            *cmd = ZDT_CMD_READ_STATUS;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_CONF:
            *cmd = ZDT_CMD_READ_CONFIG;
            *aux = 0x6CU;
            *has_aux = true;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_STATE:
            *cmd = ZDT_CMD_READ_SYSTEM_STATE;
            *aux = 0x7AU;
            *has_aux = true;
            return ERRCODE_SUCC;
        case ZDT_SYS_PARAM_ORG:
            *cmd = ZDT_CMD_READ_HOME_STATUS;
            return ERRCODE_SUCC;
        default:
            return ERRCODE_INVALID_PARAM;
    }
}

size_t zdt_build_enable_frame(uint8_t addr, bool enable, bool sync, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 6U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_ENABLE;
    frame[2] = 0xAB;
    frame[3] = enable ? 0x01U : 0x00U;
    frame[4] = sync ? 0x01U : 0x00U;
    return zdt_finalize_frame(frame, frame_size, 5U);
}

size_t zdt_build_speed_frame(
    uint8_t addr, zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, bool sync, uint8_t *frame,
    size_t frame_size)
{
    if (frame == NULL || frame_size < 8U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_SPEED;
    frame[2] = (uint8_t)direction;
    frame[3] = (uint8_t)((speed_rpm >> 8) & 0xFFU);
    frame[4] = (uint8_t)(speed_rpm & 0xFFU);
    frame[5] = accel_level;
    frame[6] = sync ? 0x01U : 0x00U;
    return zdt_finalize_frame(frame, frame_size, 7U);
}

size_t zdt_build_position_frame(
    uint8_t addr, zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, uint32_t pulses,
    zdt_position_mode_t position_mode, bool sync, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 13U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_POSITION;
    frame[2] = (uint8_t)direction;
    frame[3] = (uint8_t)((speed_rpm >> 8) & 0xFFU);
    frame[4] = (uint8_t)(speed_rpm & 0xFFU);
    frame[5] = accel_level;
    frame[6] = (uint8_t)((pulses >> 24) & 0xFFU);
    frame[7] = (uint8_t)((pulses >> 16) & 0xFFU);
    frame[8] = (uint8_t)((pulses >> 8) & 0xFFU);
    frame[9] = (uint8_t)(pulses & 0xFFU);
    frame[10] = (uint8_t)position_mode;
    frame[11] = sync ? 0x01U : 0x00U;
    return zdt_finalize_frame(frame, frame_size, 12U);
}

size_t zdt_build_stop_now_frame(uint8_t addr, bool sync, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 5U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_STOP_NOW;
    frame[2] = 0x98;
    frame[3] = sync ? 0x01U : 0x00U;
    return zdt_finalize_frame(frame, frame_size, 4U);
}

size_t zdt_build_sync_start_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 4U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_SYNC_START;
    frame[2] = 0x66;
    return zdt_finalize_frame(frame, frame_size, 3U);
}

size_t zdt_build_set_single_turn_zero_frame(uint8_t addr, bool persist, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 5U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_SET_SINGLE_TURN_ZERO;
    frame[2] = 0x88;
    frame[3] = persist ? 0x01U : 0x00U;
    return zdt_finalize_frame(frame, frame_size, 4U);
}

size_t zdt_build_clear_position_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 4U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_CLEAR_POSITION;
    frame[2] = 0x6D;
    return zdt_finalize_frame(frame, frame_size, 3U);
}

size_t zdt_build_release_stall_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 4U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_RELEASE_STALL;
    frame[2] = 0x52;
    return zdt_finalize_frame(frame, frame_size, 3U);
}

size_t zdt_build_read_sys_params_frame(uint8_t addr, zdt_sys_param_t param, uint8_t *frame, size_t frame_size)
{
    uint8_t cmd = 0;
    uint8_t aux = 0;
    bool has_aux = false;
    errcode_t ret = zdt_get_sys_param_command(param, &cmd, &aux, &has_aux);

    if (ret != ERRCODE_SUCC || frame == NULL) {
        return 0;
    }
    if (frame_size < (has_aux ? 4U : 3U)) {
        return 0;
    }

    frame[0] = addr;
    frame[1] = cmd;
    if (has_aux) {
        frame[2] = aux;
        return zdt_finalize_frame(frame, frame_size, 3U);
    }
    return zdt_finalize_frame(frame, frame_size, 2U);
}

size_t zdt_build_modify_control_mode_frame(
    uint8_t addr, bool persist, zdt_control_mode_t control_mode, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 6U) {
        return 0;
    }

    frame[0] = addr;
    frame[1] = ZDT_CMD_MODIFY_CONTROL_MODE;
    frame[2] = 0x69U;
    frame[3] = persist ? 0x01U : 0x00U;
    frame[4] = (uint8_t)control_mode;
    return zdt_finalize_frame(frame, frame_size, 5U);
}

size_t zdt_build_trigger_home_frame(uint8_t addr, zdt_home_mode_t mode, bool sync, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 5U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_TRIGGER_HOME;
    frame[2] = (uint8_t)mode;
    frame[3] = sync ? 0x01U : 0x00U;
    return zdt_finalize_frame(frame, frame_size, 4U);
}

size_t zdt_build_modify_home_params_frame(
    uint8_t addr, bool persist, const zdt_home_params_t *home_params, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || home_params == NULL || frame_size < 20U) {
        return 0;
    }

    frame[0] = addr;
    frame[1] = ZDT_CMD_MODIFY_HOME_PARAMS;
    frame[2] = 0xAEU;
    frame[3] = persist ? 0x01U : 0x00U;
    frame[4] = (uint8_t)home_params->home_mode;
    frame[5] = (uint8_t)home_params->direction;
    frame[6] = (uint8_t)((home_params->home_speed_rpm >> 8) & 0xFFU);
    frame[7] = (uint8_t)(home_params->home_speed_rpm & 0xFFU);
    frame[8] = (uint8_t)((home_params->timeout_ms >> 24) & 0xFFU);
    frame[9] = (uint8_t)((home_params->timeout_ms >> 16) & 0xFFU);
    frame[10] = (uint8_t)((home_params->timeout_ms >> 8) & 0xFFU);
    frame[11] = (uint8_t)(home_params->timeout_ms & 0xFFU);
    frame[12] = (uint8_t)((home_params->stall_speed_rpm >> 8) & 0xFFU);
    frame[13] = (uint8_t)(home_params->stall_speed_rpm & 0xFFU);
    frame[14] = (uint8_t)((home_params->stall_current_ma >> 8) & 0xFFU);
    frame[15] = (uint8_t)(home_params->stall_current_ma & 0xFFU);
    frame[16] = (uint8_t)((home_params->stall_time_ms >> 8) & 0xFFU);
    frame[17] = (uint8_t)(home_params->stall_time_ms & 0xFFU);
    frame[18] = home_params->power_on_trigger ? 0x01U : 0x00U;
    return zdt_finalize_frame(frame, frame_size, 19U);
}

size_t zdt_build_abort_home_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 4U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_ABORT_HOME;
    frame[2] = 0x48;
    return zdt_finalize_frame(frame, frame_size, 3U);
}

size_t zdt_build_read_version_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 3U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_READ_VERSION;
    return zdt_finalize_frame(frame, frame_size, 2U);
}

size_t zdt_build_read_status_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 3U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_READ_STATUS;
    return zdt_finalize_frame(frame, frame_size, 2U);
}

size_t zdt_build_read_home_status_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 3U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_READ_HOME_STATUS;
    return zdt_finalize_frame(frame, frame_size, 2U);
}

size_t zdt_build_read_real_speed_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 3U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_READ_REAL_SPEED;
    return zdt_finalize_frame(frame, frame_size, 2U);
}

size_t zdt_build_read_real_position_frame(uint8_t addr, uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < 3U) {
        return 0;
    }
    frame[0] = addr;
    frame[1] = ZDT_CMD_READ_REAL_POSITION;
    return zdt_finalize_frame(frame, frame_size, 2U);
}

errcode_t zdt_parse_simple_ack(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, uint8_t *status)
{
    errcode_t ret = zdt_validate_header(frame, frame_len, addr, cmd, 4U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    if (status != NULL) {
        *status = frame[2];
    }
    if (frame[2] == ZDT_PROTOCOL_STATUS_OK) {
        return ERRCODE_SUCC;
    }
    if (frame[2] == ZDT_PROTOCOL_STATUS_REJECTED) {
        return ZDT_ERR_REMOTE_REJECTED;
    }
    return ZDT_ERR_REMOTE_ERROR;
}

errcode_t zdt_parse_u8_reply(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, uint8_t *value)
{
    errcode_t ret = zdt_validate_header(frame, frame_len, addr, cmd, 4U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    if (value == NULL) {
        return ERRCODE_INVALID_PARAM;
    }
    *value = frame[2];
    return ERRCODE_SUCC;
}

errcode_t zdt_parse_signed16_reply(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, int16_t *value)
{
    int32_t magnitude;
    errcode_t ret = zdt_validate_header(frame, frame_len, addr, cmd, 6U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    if (value == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    magnitude = (int32_t)(((uint16_t)frame[3] << 8) | (uint16_t)frame[4]);
    *value = (frame[2] != 0U) ? (int16_t)(-magnitude) : (int16_t)magnitude;
    return ERRCODE_SUCC;
}

errcode_t zdt_parse_signed32_reply(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, int32_t *value)
{
    uint32_t magnitude;
    errcode_t ret = zdt_validate_header(frame, frame_len, addr, cmd, 8U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    if (value == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    magnitude = ((uint32_t)frame[3] << 24) | ((uint32_t)frame[4] << 16) | ((uint32_t)frame[5] << 8) |
                (uint32_t)frame[6];
    if (frame[2] != 0U) {
        *value = -(int32_t)magnitude;
    } else {
        *value = (int32_t)magnitude;
    }
    return ERRCODE_SUCC;
}

errcode_t zdt_parse_version_reply(
    const uint8_t *frame, size_t frame_len, uint8_t addr, zdt_version_info_t *version_info)
{
    errcode_t ret = zdt_validate_header(frame, frame_len, addr, ZDT_CMD_READ_VERSION, 5U);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    if (version_info == NULL) {
        return ERRCODE_INVALID_PARAM;
    }
    version_info->firmware_version = frame[2];
    version_info->hardware_version = frame[3];
    return ERRCODE_SUCC;
}
