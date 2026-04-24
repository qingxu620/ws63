/**
 * @file main.c
 * @brief WS63 ZDT 控制板主入口
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "soc_osal.h"

#include "Emm_V5.h"
#include "zdt_controller.h"

static int zdt_demo_task(void *arg)
{
    uint8_t motor_status = 0;
    uint8_t home_status = 0;
    int16_t speed_rpm = 0;
    int32_t position_raw = 0;
    zdt_version_info_t version_info = {0};
    errcode_t ret;

    unused(arg);

    osal_msleep(200);

    Emm_V5_Read_Sys_Params(ZDT_DEVICE_ADDR, S_VER);
    ret = Emm_V5_Get_Last_Result();
    if (ret == ERRCODE_SUCC && rxFrameFlag && rxCount >= 5U) {
        osal_printk(
            "[zdt] official api version OK, firmware=0x%02x, hardware=0x%02x\r\n", rxCmd[2], rxCmd[3]);
    } else {
        ret = zdt_controller_read_version(&version_info);
        if (ret == ERRCODE_SUCC) {
            osal_printk(
                "[zdt] controller version OK, firmware=0x%02x, hardware=0x%02x\r\n", version_info.firmware_version,
                version_info.hardware_version);
        } else {
            osal_printk("[zdt] read version failed: 0x%x\r\n", ret);
        }
    }

#if ZDT_DEMO_AUTO_RUN
    Emm_V5_En_Control(ZDT_DEVICE_ADDR, true, false);
    ret = Emm_V5_Get_Last_Result();
    osal_printk("[zdt] official api enable ret=0x%x status=0x%02x\r\n", ret, rxCount >= 3U ? rxCmd[2] : 0U);
    osal_msleep(100);

    Emm_V5_Reset_CurPos_To_Zero(ZDT_DEVICE_ADDR);
    ret = Emm_V5_Get_Last_Result();
    osal_printk("[zdt] official api clear pos ret=0x%x status=0x%02x\r\n", ret, rxCount >= 3U ? rxCmd[2] : 0U);
    osal_msleep(100);

    Emm_V5_Pos_Control(
        ZDT_DEVICE_ADDR, ZDT_DIR_CW, ZDT_DEMO_MOVE_SPEED_RPM, ZDT_DEMO_MOVE_ACCEL_LEVEL, ZDT_DEMO_MOVE_PULSES, false,
        false);
    ret = Emm_V5_Get_Last_Result();
    osal_printk(
        "[zdt] official api move ret=0x%x status=0x%02x speed=%u acc=%u pulses=%u\r\n", ret,
        rxCount >= 3U ? rxCmd[2] : 0U, (unsigned)ZDT_DEMO_MOVE_SPEED_RPM, (unsigned)ZDT_DEMO_MOVE_ACCEL_LEVEL,
        (unsigned)ZDT_DEMO_MOVE_PULSES);
    osal_msleep(100);
#endif

    while (1) {
        ret = zdt_controller_read_motor_status(&motor_status);
        if (ret != ERRCODE_SUCC) {
            osal_printk("[zdt] read motor status failed: 0x%x\r\n", ret);
        }

        ret = zdt_controller_read_home_status(&home_status);
        if (ret != ERRCODE_SUCC) {
            osal_printk("[zdt] read home status failed: 0x%x\r\n", ret);
        }

        ret = zdt_controller_read_real_speed(&speed_rpm);
        if (ret != ERRCODE_SUCC) {
            osal_printk("[zdt] read speed failed: 0x%x\r\n", ret);
        }

        ret = zdt_controller_read_real_position(&position_raw);
        if (ret != ERRCODE_SUCC) {
            osal_printk("[zdt] read position failed: 0x%x\r\n", ret);
        }

        osal_printk(
            "[zdt] state motor=0x%02x home=0x%02x speed=%dRPM pos_raw=%ld\r\n", motor_status, home_status,
            (int)speed_rpm, (long)position_raw);
        osal_msleep(ZDT_POLL_INTERVAL_MS);
    }

    return OSAL_SUCCESS;
}

static void zdt_controller_entry(void)
{
    zdt_uart_config_t uart_config = {
        .uart_bus = (uart_bus_t)ZDT_UART_BUS,
        .tx_pin = ZDT_UART_TX_PIN,
        .rx_pin = ZDT_UART_RX_PIN,
        .pin_mode = ZDT_UART_PIN_MODE,
        .baud_rate = ZDT_UART_BAUD_RATE,
        .rs485_dir_enable = (ZDT_RS485_DIR_ENABLE != 0),
        .rs485_dir_pin = ZDT_RS485_DIR_PIN,
        .rs485_dir_active_high = (ZDT_RS485_DIR_ACTIVE_HIGH != 0),
    };
    zdt_controller_config_t controller_config = {
        .device_addr = ZDT_DEVICE_ADDR,
        .reply_timeout_ms = ZDT_UART_REPLY_TIMEOUT_MS,
    };
    osal_task *task = NULL;
    errcode_t ret;

    osal_printk("========================================\r\n");
    osal_printk("  WS63 ZDT Controller Board\r\n");
    osal_printk("========================================\r\n");

    ret = zdt_controller_init(&uart_config, &controller_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[zdt] controller init failed: 0x%x\r\n", ret);
        return;
    }

    osal_kthread_lock();
    task = osal_kthread_create(zdt_demo_task, NULL, "zdt_demo", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[zdt] create demo task failed\r\n");
        return;
    }
    osal_kthread_set_priority(task, TASK_PRIO_DEFAULT);
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[zdt] demo task created\r\n");
}

app_run(zdt_controller_entry);
