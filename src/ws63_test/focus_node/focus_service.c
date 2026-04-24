/**
 * @file focus_service.c
 * @brief 感知与对焦节点业务层
 */
#include "focus_service.h"
#include <string.h>

static focus_service_state_t g_focus_state;

static void focus_service_apply_flags(void)
{
    g_focus_state.z_enabled = (g_focus_state.motor_flags & 0x01U) != 0U;
    g_focus_state.z_in_position = (g_focus_state.motor_flags & 0x02U) != 0U;
    g_focus_state.z_homed = ((g_focus_state.home_flags & 0x01U) != 0U) && ((g_focus_state.home_flags & 0x02U) != 0U);
    if (!g_focus_state.z_link_ready) {
        g_focus_state.status_code = FOCUS_STATUS_ERROR;
    } else if ((g_focus_state.z_speed_rpm != 0) || !g_focus_state.z_in_position) {
        g_focus_state.status_code = FOCUS_STATUS_BUSY;
    } else {
        g_focus_state.status_code = FOCUS_STATUS_IDLE;
    }
}

static errcode_t focus_service_record_error(errcode_t ret, uint8_t focus_error)
{
    if (ret != ERRCODE_SUCC) {
        g_focus_state.status_code = FOCUS_STATUS_ERROR;
        g_focus_state.error_code = focus_error;
        g_focus_state.z_link_ready = false;
    }
    return ret;
}

errcode_t focus_service_init(const focus_service_config_t *config)
{
    uint8_t ack_status = 0;
    errcode_t ret;

    if (config == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    memset(&g_focus_state, 0, sizeof(g_focus_state));
    g_focus_state.status_code = FOCUS_STATUS_IDLE;
    g_focus_state.error_code = FOCUS_ERR_NONE;

    ret = zdt_controller_init(&config->zdt_uart, &config->zdt_controller);
    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_NOT_READY);
    }

    g_focus_state.z_link_ready = true;

    if (config->boot_enable_z) {
        ret = zdt_controller_enable_motor(true, false, &ack_status);
        if (ret != ERRCODE_SUCC) {
            return focus_service_record_error(ret, FOCUS_ERR_Z_MOVE_REJECTED);
        }
    }

    if (config->boot_sync_zero) {
        ret = zdt_controller_clear_position(&ack_status);
        if (ret != ERRCODE_SUCC) {
            return focus_service_record_error(ret, FOCUS_ERR_Z_MOVE_REJECTED);
        }
    }

    return focus_service_poll();
}

errcode_t focus_service_poll(void)
{
    errcode_t ret;
    uint8_t motor_flags = 0;
    uint8_t home_flags = 0;
    int16_t speed_rpm = 0;
    int32_t position_pulses = 0;

    ret = zdt_controller_read_motor_status(&motor_flags);
    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_NOT_READY);
    }

    ret = zdt_controller_read_home_status(&home_flags);
    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_NOT_READY);
    }

    ret = zdt_controller_read_real_speed(&speed_rpm);
    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_NOT_READY);
    }

    ret = zdt_controller_read_real_position(&position_pulses);
    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_NOT_READY);
    }

    g_focus_state.z_link_ready = true;
    g_focus_state.motor_flags = motor_flags;
    g_focus_state.home_flags = home_flags;
    g_focus_state.z_speed_rpm = speed_rpm;
    g_focus_state.z_position_pulses = position_pulses;
    g_focus_state.error_code = FOCUS_ERR_NONE;
    focus_service_apply_flags();
    return ERRCODE_SUCC;
}

errcode_t focus_service_get_state(focus_service_state_t *state)
{
    if (state == NULL) {
        return ERRCODE_INVALID_PARAM;
    }
    *state = g_focus_state;
    return ERRCODE_SUCC;
}

errcode_t focus_service_enable_z(bool enable)
{
    uint8_t ack_status = 0;
    errcode_t ret;

    ret = zdt_controller_enable_motor(enable, false, &ack_status);
    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_MOVE_REJECTED);
    }
    return focus_service_poll();
}

errcode_t focus_service_stop_z(void)
{
    uint8_t ack_status = 0;
    errcode_t ret = zdt_controller_stop_now(false, &ack_status);

    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_MOVE_REJECTED);
    }
    return focus_service_poll();
}

errcode_t focus_service_move_z_abs_pulses(uint32_t target_pulses, uint16_t speed_rpm, uint8_t accel_level)
{
    uint8_t ack_status = 0;
    zdt_direction_t direction =
        ((int32_t)target_pulses >= g_focus_state.z_position_pulses) ? ZDT_DIR_CW : ZDT_DIR_CCW;
    errcode_t ret = zdt_controller_run_position(
        direction, speed_rpm, accel_level, target_pulses, ZDT_POSITION_ABSOLUTE, false, &ack_status);

    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_MOVE_REJECTED);
    }
    g_focus_state.status_code = FOCUS_STATUS_BUSY;
    return ERRCODE_SUCC;
}

errcode_t focus_service_move_z_rel_pulses(
    zdt_direction_t direction, uint32_t delta_pulses, uint16_t speed_rpm, uint8_t accel_level)
{
    uint8_t ack_status = 0;
    errcode_t ret = zdt_controller_run_position(
        direction, speed_rpm, accel_level, delta_pulses, ZDT_POSITION_RELATIVE, false, &ack_status);

    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_Z_MOVE_REJECTED);
    }
    g_focus_state.status_code = FOCUS_STATUS_BUSY;
    return ERRCODE_SUCC;
}

errcode_t focus_service_home_z(zdt_home_mode_t home_mode)
{
    uint8_t ack_status = 0;
    errcode_t ret = zdt_controller_trigger_home(home_mode, false, &ack_status);

    if (ret != ERRCODE_SUCC) {
        return focus_service_record_error(ret, FOCUS_ERR_HOME_REJECTED);
    }
    g_focus_state.status_code = FOCUS_STATUS_BUSY;
    return ERRCODE_SUCC;
}

errcode_t focus_service_measure_height(int32_t *height_raw)
{
    if (height_raw != NULL) {
        *height_raw = g_focus_state.height_raw;
    }
    g_focus_state.error_code = FOCUS_ERR_HEIGHT_NOT_READY;
    return ERRCODE_NOT_SUPPORT;
}

errcode_t focus_service_autofocus(void)
{
    g_focus_state.error_code = FOCUS_ERR_NOT_SUPPORTED;
    return ERRCODE_NOT_SUPPORT;
}
