/**
 * @file zdt_protocol.h
 * @brief 张大头 Emm_V5.0 自定义串口协议封装
 */
#ifndef WS63_ZDT_PROTOCOL_H
#define WS63_ZDT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZDT_ERR_REPLY_INVALID 0xA0630001UL
#define ZDT_ERR_REMOTE_REJECTED 0xA0630002UL
#define ZDT_ERR_REMOTE_ERROR 0xA0630003UL
#define ZDT_ERR_BUFFER_TOO_SMALL 0xA0630004UL

#define ZDT_PROTOCOL_CHECKSUM_FIXED 0x6BU
#define ZDT_PROTOCOL_STATUS_OK 0x02U
#define ZDT_PROTOCOL_STATUS_REJECTED 0xE2U
#define ZDT_PROTOCOL_STATUS_BAD_COMMAND 0xEEU

typedef enum {
    ZDT_DIR_CW = 0,
    ZDT_DIR_CCW = 1,
} zdt_direction_t;

typedef enum {
    ZDT_POSITION_RELATIVE = 0,
    ZDT_POSITION_ABSOLUTE = 1,
} zdt_position_mode_t;

typedef enum {
    ZDT_HOME_NEAREST = 0,
    ZDT_HOME_DIRECTION = 1,
    ZDT_HOME_COLLISION = 2,
    ZDT_HOME_LIMIT_SWITCH = 3,
} zdt_home_mode_t;

typedef enum {
    ZDT_SYS_PARAM_VER = 0,
    ZDT_SYS_PARAM_RL = 1,
    ZDT_SYS_PARAM_PID = 2,
    ZDT_SYS_PARAM_VBUS = 3,
    ZDT_SYS_PARAM_CPHA = 5,
    ZDT_SYS_PARAM_ENCL = 7,
    ZDT_SYS_PARAM_TPOS = 8,
    ZDT_SYS_PARAM_VEL = 9,
    ZDT_SYS_PARAM_CPOS = 10,
    ZDT_SYS_PARAM_PERR = 11,
    ZDT_SYS_PARAM_FLAG = 13,
    ZDT_SYS_PARAM_CONF = 14,
    ZDT_SYS_PARAM_STATE = 15,
    ZDT_SYS_PARAM_ORG = 16,
} zdt_sys_param_t;

typedef enum {
    ZDT_CTRL_MODE_DISABLE_PULSE = 0,
    ZDT_CTRL_MODE_OPEN_LOOP = 1,
    ZDT_CTRL_MODE_CLOSED_LOOP = 2,
    ZDT_CTRL_MODE_IO_REMAP = 3,
} zdt_control_mode_t;

typedef struct {
    uint8_t firmware_version;
    uint8_t hardware_version;
} zdt_version_info_t;

typedef struct {
    zdt_home_mode_t home_mode;
    zdt_direction_t direction;
    uint16_t home_speed_rpm;
    uint32_t timeout_ms;
    uint16_t stall_speed_rpm;
    uint16_t stall_current_ma;
    uint16_t stall_time_ms;
    bool power_on_trigger;
} zdt_home_params_t;

size_t zdt_build_enable_frame(uint8_t addr, bool enable, bool sync, uint8_t *frame, size_t frame_size);
size_t zdt_build_speed_frame(
    uint8_t addr, zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, bool sync, uint8_t *frame,
    size_t frame_size);
size_t zdt_build_position_frame(
    uint8_t addr, zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, uint32_t pulses,
    zdt_position_mode_t position_mode, bool sync, uint8_t *frame, size_t frame_size);
size_t zdt_build_stop_now_frame(uint8_t addr, bool sync, uint8_t *frame, size_t frame_size);
size_t zdt_build_sync_start_frame(uint8_t addr, uint8_t *frame, size_t frame_size);
size_t zdt_build_set_single_turn_zero_frame(uint8_t addr, bool persist, uint8_t *frame, size_t frame_size);
size_t zdt_build_clear_position_frame(uint8_t addr, uint8_t *frame, size_t frame_size);
size_t zdt_build_release_stall_frame(uint8_t addr, uint8_t *frame, size_t frame_size);
size_t zdt_build_read_sys_params_frame(uint8_t addr, zdt_sys_param_t param, uint8_t *frame, size_t frame_size);
size_t zdt_build_modify_control_mode_frame(
    uint8_t addr, bool persist, zdt_control_mode_t control_mode, uint8_t *frame, size_t frame_size);
size_t zdt_build_trigger_home_frame(uint8_t addr, zdt_home_mode_t mode, bool sync, uint8_t *frame, size_t frame_size);
size_t zdt_build_modify_home_params_frame(
    uint8_t addr, bool persist, const zdt_home_params_t *home_params, uint8_t *frame, size_t frame_size);
size_t zdt_build_abort_home_frame(uint8_t addr, uint8_t *frame, size_t frame_size);

size_t zdt_build_read_version_frame(uint8_t addr, uint8_t *frame, size_t frame_size);
size_t zdt_build_read_status_frame(uint8_t addr, uint8_t *frame, size_t frame_size);
size_t zdt_build_read_home_status_frame(uint8_t addr, uint8_t *frame, size_t frame_size);
size_t zdt_build_read_real_speed_frame(uint8_t addr, uint8_t *frame, size_t frame_size);
size_t zdt_build_read_real_position_frame(uint8_t addr, uint8_t *frame, size_t frame_size);

errcode_t zdt_parse_simple_ack(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, uint8_t *status);
errcode_t zdt_parse_u8_reply(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, uint8_t *value);
errcode_t zdt_parse_signed16_reply(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, int16_t *value);
errcode_t zdt_parse_signed32_reply(const uint8_t *frame, size_t frame_len, uint8_t addr, uint8_t cmd, int32_t *value);
errcode_t zdt_parse_version_reply(
    const uint8_t *frame, size_t frame_len, uint8_t addr, zdt_version_info_t *version_info);

#ifdef __cplusplus
}
#endif

#endif
