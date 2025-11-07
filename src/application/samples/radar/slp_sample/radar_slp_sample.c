/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2024-2025. All rights reserved.
 *
 * Description: Radar SLP samples function \n
 *
 */

#include "stdlib.h"
#include "uart.h"
#include "td_base.h"
#include "td_type.h"
#include "cmsis_os2.h"
#include "soc_osal.h"
#include "app_init.h"
#include "osal_task.h"
#include "securec.h"
#include "slp_radar_service.h"

#define SLP_SAMPLE_RPT_TARGET_NUM   1

#define RADAR_DEFAULT_BOARD_ID  4
#define RADAR_DEFAULT_TX_POWER_IDX  38
#define RADAR_DEFAULT_RPT_MODE  0
#define RADAR_DEFAULT_SENSITIVITY   0
#define RADAR_DEFAULT_RNG_BOUNDARY  700
#define RADAR_DEFAULT_DLY_TIME  20

static void slp_radar_init_para(void)
{
    slp_radar_hardware_para_t hd_para;
    hd_para.board_id = RADAR_DEFAULT_BOARD_ID;
    hd_para.tx_power_idx = RADAR_DEFAULT_TX_POWER_IDX;
    uapi_slp_radar_set_hardware_para(&hd_para);

    slp_radar_alg_para_t alg_para;
    alg_para.rpt_mode = RADAR_DEFAULT_RPT_MODE;
    alg_para.sensitivity = RADAR_DEFAULT_SENSITIVITY;
    alg_para.rng_boundary = RADAR_DEFAULT_RNG_BOUNDARY;
    alg_para.delay_time = RADAR_DEFAULT_DLY_TIME;
    uapi_slp_radar_set_alg_para(&alg_para);
}

void slp_radar_auto_start(void)
{
    uapi_slp_radar_set_status(RADAR_STATUS_START);
    slp_radar_init_para();
}

static void slp_radar_print_res(const slp_radar_result_t *result)
{
    for (uint32_t i = 0; i < SLP_SAMPLE_RPT_TARGET_NUM; i++) {
        osal_printk("[SLP_RADAR][SAMPLE] %u, %d, %d, %d\r\n", result->target_info[i].id, result->target_info[i].rng,
            result->target_info[i].agl, result->target_info[i].vel);
    }
}

static void slp_radar_print_set_sts_cb_res(uint8_t status, errcode_slp_radar_client_t err_code)
{
    if (err_code == ERRCODE_SLPRC_SUCCESS) {
        osal_printk("[SLP_RADAR][SAMPLE] Radar set status %u succ.\r\n", status);
    } else {
        osal_printk("[SLP_RADAR][SAMPLE] Radar set status %u fail, errcode:0x%x.\r\n", status, err_code);
    }
}

void radar_sample_pc_uart_init(void)
{
    uapi_slp_radar_register_result_cb(slp_radar_print_res);
    uapi_slp_radar_register_set_status_cb(slp_radar_print_set_sts_cb_res);
    slp_radar_auto_start();
}