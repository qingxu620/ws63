/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: slp radar errcode.
 */

#ifndef SLP_RADAR_ERRCODE_H
#define SLP_RADAR_ERRCODE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @if Eng
 * @brief  SLP radar error code base.
 * @else
 * @brief  SLP雷达错误码起始。
 * @endif
 */
#define ERRCODE_SLP_RADAR_BASE 0x8000A000

/**
 * @if Eng
 * @brief  SLP radar error code base.
 * @else
 * @brief  SLP雷达错误码结束。
 * @endif
 */
#define ERRCODE_SLP_RADAR_END  0x8000A0FF

/**
 * @if Eng
 * @brief  SLP radar client error code base.
 * @else
 * @brief  SLP雷达client错误码起始。
 * @endif
 */
#define ERRCODE_SLP_RADAR_CLIENT_BASE 0x8000A100

/**
 * @if Eng
 * @brief  SLP radar Client error code base.
 * @else
 * @brief  SLP雷达client错误码结束。
 * @endif
 */
#define ERRCODE_SLP_RADAR_CLIENT_END  0x8000A1FF

/**
 * @if Eng
 * @brief  SLP radar error code.
 * @else
 * @brief  SLP雷达错误码。
 * @endif
 */
typedef enum {
    ERRCODE_SLPR_SUCCESS = 0,                               /*!< @if Eng error code of success
                                                                @else   执行成功错误码 @endif */
    ERRCODE_SLPR_MAX = ERRCODE_SLP_RADAR_END                /*!< @if Eng maximum of SLP error code
                                                                @else   SLP雷达错误码最大值 @endif */
} errcode_slp_radar_t;

/**
 * @if Eng
 * @brief  SLP radar client error code.
 * @else
 * @brief  SLP雷达client错误码。
 * @endif
 */
typedef enum {
    ERRCODE_SLPRC_SUCCESS = 0,                                  /*!< @if Eng error code of success
                                                                    @else   执行成功错误码 @endif */
    ERRCODE_SLPRC_MALLOC_FAILED = ERRCODE_SLP_RADAR_CLIENT_BASE,/*!< @if Eng error code of malloc failed
                                                                    @else   申请动态内存失败错误码 @endif */
    ERRCODE_SLPRC_QUEUE_WRITE_FAILED,                           /*!< @if Eng error code of message queue write failed
                                                                    @else   消息队列写入失败错误码 @endif */
    ERRCODE_SLPRC_POWER_ON_FAILED,                              /*!< @if Eng error code of power on failed
                                                                    @else   上电加载失败错误码 @endif */
    ERRCODE_SLPRC_POWERED_ON,                                   /*!< @if Eng error code of SLP is powerd on
                                                                    @else   SLP已经上电加载错误码 @endif */
    ERRCODE_SLPRC_POWERED_OFF,                                  /*!< @if Eng error code of SLP is powerd off
                                                                    @else   SLP已经下电错误码 @endif */
    ERRCODE_SLPRC_MAX = ERRCODE_SLP_RADAR_CLIENT_END            /*!< @if Eng maximum of SLP error code
                                                                    @else   SLPRC错误码最大值 @endif */
} errcode_slp_radar_client_t;

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif
