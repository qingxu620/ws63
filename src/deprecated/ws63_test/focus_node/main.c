/**
 * @file main.c
 * @brief WS63 感知与对焦节点主入口（第一版：TTL UART + ZDT Z轴）
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "soc_osal.h"

#include "focus_service.h"
#include "focus_sle_server.h"

static int focus_sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    if (focus_sle_server_init() != ERRCODE_SUCC) {
        osal_printk("[focus node] focus sle init failed\r\n");
    }
    return OSAL_SUCCESS;
}

static int focus_node_task(void *arg)
{
    focus_service_state_t state = {0};
    errcode_t ret;

    unused(arg);

#if ZDT_DEMO_AUTO_RUN
    osal_msleep(200);
    ret = focus_service_move_z_rel_pulses(
        ZDT_DIR_CW, ZDT_DEMO_MOVE_PULSES, ZDT_DEMO_MOVE_SPEED_RPM, ZDT_DEMO_MOVE_ACCEL_LEVEL);
    osal_printk(
        "[focus node] demo move ret=0x%x pulses=%u speed=%u acc=%u\r\n", ret, (unsigned)ZDT_DEMO_MOVE_PULSES,
        (unsigned)ZDT_DEMO_MOVE_SPEED_RPM, (unsigned)ZDT_DEMO_MOVE_ACCEL_LEVEL);
#endif

    while (1) {
        ret = focus_service_poll();
        if (ret != ERRCODE_SUCC) {
            osal_printk("[focus node] poll failed: 0x%x\r\n", ret);
        } else if (focus_service_get_state(&state) == ERRCODE_SUCC) {
            (void)focus_sle_server_publish_state(&state);
            osal_printk(
                "[focus node] z_ready=%u z_en=%u z_in_pos=%u z_homed=%u motor=0x%02x home=0x%02x speed=%d pos=%ld\r\n",
                state.z_link_ready ? 1U : 0U, state.z_enabled ? 1U : 0U, state.z_in_position ? 1U : 0U,
                state.z_homed ? 1U : 0U, state.motor_flags, state.home_flags, (int)state.z_speed_rpm,
                (long)state.z_position_pulses);
        }
        osal_msleep(ZDT_POLL_INTERVAL_MS);
    }

    return OSAL_SUCCESS;
}

static void focus_node_entry(void)
{
    focus_service_config_t config = {
        .zdt_uart =
            {
                .uart_bus = (uart_bus_t)ZDT_UART_BUS,
                .tx_pin = ZDT_UART_TX_PIN,
                .rx_pin = ZDT_UART_RX_PIN,
                .pin_mode = ZDT_UART_PIN_MODE,
                .baud_rate = ZDT_UART_BAUD_RATE,
                .rs485_dir_enable = (ZDT_RS485_DIR_ENABLE != 0),
                .rs485_dir_pin = ZDT_RS485_DIR_PIN,
                .rs485_dir_active_high = (ZDT_RS485_DIR_ACTIVE_HIGH != 0),
            },
        .zdt_controller =
            {
                .device_addr = ZDT_DEVICE_ADDR,
                .reply_timeout_ms = ZDT_UART_REPLY_TIMEOUT_MS,
            },
        .boot_enable_z = (FOCUS_NODE_BOOT_ENABLE_Z != 0),
        .boot_sync_zero = (FOCUS_NODE_BOOT_SYNC_ZERO != 0),
    };
    osal_task *task = NULL;
    errcode_t ret;

    osal_printk("========================================\r\n");
    osal_printk("  WS63 Focus Node Board\r\n");
    osal_printk("========================================\r\n");
    osal_printk("[focus node] current Z-axis path: UART1(GPIO15/16) TTL -> Emm_V5.0\r\n");

    ret = focus_service_init(&config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[focus node] init failed: 0x%x\r\n", ret);
        return;
    }

    osal_kthread_lock();
    task = osal_kthread_create(focus_node_task, NULL, "focus_node", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[focus node] create task failed\r\n");
        return;
    }
    osal_kthread_set_priority(task, TASK_PRIO_DEFAULT);
    osal_kfree(task);

    task = osal_kthread_create(focus_sle_init_task, NULL, "focus_sle", TASK_STACK_SIZE_SLE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[focus node] create focus_sle task failed\r\n");
        return;
    }
    osal_kthread_set_priority(task, TASK_PRIO_SLE);
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[focus node] task created\r\n");
}

app_run(focus_node_entry);
