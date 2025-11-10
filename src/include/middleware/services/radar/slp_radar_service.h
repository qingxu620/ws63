/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: header file for slp radar service api.
 */

#ifndef SLP_RADAR_SERVICE_H
#define SLP_RADAR_SERVICE_H

#include <stdint.h>
#include "slp_radar_errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_RPT_TARGET_NUM  3

/**
 * @if Eng
 * @brief  radar status set.
 * @else
 * @brief  雷达状态配置。
 * @endif
 */
typedef enum {
    RADAR_STATUS_STOP = 0,  /*!< @if Eng radar status set: stop
                                @else   雷达状态配置停止 @endif */
    RADAR_STATUS_START,     /*!< @if Eng radar status set: start
                                @else   雷达状态配置启动 @endif */
} radar_set_status_t;

/**
 * @if Eng
 * @brief  radar target detection result.
 * @else
 * @brief  雷达目标检测结果信息。
 * @endif
 */
typedef struct {
    uint16_t id;                /*!< @if Eng radar detection res: target id
                                    @else   雷达检测结果：目标序号 @endif */
    int16_t rng;                /*!< @if Eng radar detection res: target range(m * 100)
                                    @else   雷达检测结果：目标距离(米 * 100） @endif */
    int16_t agl;                /*!< @if Eng radar detection res: target angle(degree * 100)
                                    @else   雷达检测结果：目标角度(度 * 100) @endif */
    int16_t vel;                /*!< @if Eng radar detection res: target velocity(m/s * 10000)
                                    @else   雷达检测结果：目标速度(米/秒 * 10000) @endif */
} radar_target_info_t;

/**
 * @if Eng
 * @brief  radar results.
 * @else
 * @brief  雷达检测结果。
 * @endif
 */
typedef struct {
    radar_target_info_t target_info[MAX_RPT_TARGET_NUM];    /*!< @if Eng radar res: target detection results
                                                                @else   雷达检测结果：目标检测结果数组 @endif */
    uint8_t target_num;                                     /*!< @if Eng radar detection res: valid target num
                                                                @else   雷达检测结果：有效目标个数 @endif */
} slp_radar_result_t;

/**
 * @if Eng
 * @brief  radar hardware parameter.
 * @else
 * @brief  雷达硬件参数。
 * @endif
 */
typedef struct {
    uint32_t board_id;              /*!< @if Eng radar hardware parameter: board id
                                        @else   雷达硬件参数：模组型号 @endif */
    uint32_t tx_power_idx;          /*!< @if Eng radar hardware parameter: tx power index
                                        @else   雷达硬件参数：Tx发射功率索引 @endif */
} slp_radar_hardware_para_t;

/**
 * @if Eng
 * @brief  radar algorithm parameter.
 * @else
 * @brief  雷达算法参数。
 * @endif
 */
typedef struct {
    uint32_t rpt_mode;              /*!< @if Eng radar algorithm parameter: board id
                                        @else   雷达算法参数: 数据上报类型 @endif */
    uint32_t sensitivity;           /*!< @if Eng radar algorithm parameter: detection sensitivity
                                        @else   雷达算法参数: 检测灵敏度 @endif */
    uint32_t rng_boundary;          /*!< @if Eng radar algorithm parameter: detection range boundary
                                        @else   雷达算法参数: 距离检测边界 @endif */
    uint32_t delay_time;            /*!< @if Eng radar algorithm parameter: delay time
                                        @else   雷达算法参数: 退出时延 @endif */
} slp_radar_alg_para_t;

/**
 * @if Eng
 * @brief Callback invoked when the radar detection is complete.
 * @par Description: Callback invoked when the radar detection is complete.
 * @attention This callback function runs on the slp radar client thread.
 * @attention It cannot be blocked or wait for a long time or use a large stack space.
 * @param [in] result slp_radar_result_t.
 * @else
 * @brief  雷达检测完成的结果回调函数。
 * @par Description: 雷达检测完成的结果回调函数。
 * @attention  该回调函数运行于slp radar client线程, 不能阻塞或长时间等待, 不能使用较大栈空间。
 * @param [in] result 雷达检测结果。
 * @endif
 */
typedef void (*slp_radar_result_cb_t)(const slp_radar_result_t *result);

/**
 * @if Eng
 * @brief Callback invoked when the radar set status process is complete.
 * @par Description: Callback invoked when the radar set status process is complete.
 * @attention This callback function runs on the slp radar client thread.
 * @attention It cannot be blocked or wait for a long time or use a large stack space.
 * @param [in] status radar_set_status_t
 * @param [in] err_code errcode_slp_radar_client_t
 * @else
 * @brief  设置雷达运行状态的回调函数。
 * @par Description: 设置雷达运行状态的回调函数。
 * @attention  该回调函数运行于slp radar client线程, 不能阻塞或长时间等待, 不能使用较大栈空间。
 * @param [in] status 设置的雷达状态
 * @param [in] err_code 错误码
 * @endif
 */
typedef void (*slp_radar_set_status_cb_t)(uint8_t status, errcode_slp_radar_client_t err_code);

/**
 * @if Eng
 * @brief Callback invoked when the radar detection is complete.
 * @par Description: Callback invoked when the radar detection is complete.
 * @attention This callback function runs on the slp radar client thread.
 * @attention It cannot be blocked or wait for a long time or use a large stack space.
 * @param [in] err_code errcode_slp_radar_t
 * @else
 * @brief  SLP雷达错误码上报的回调函数。
 * @par Description: SLP雷达错误码上报的回调函数。
 * @attention  该回调函数运行于slp radar client线程, 不能阻塞或长时间等待, 不能使用较大栈空间。
 * @param [in] err_code 错误码
 * @endif
 */
typedef void (*slp_radar_report_errcode_cb_t)(errcode_slp_radar_t err_code);

/**
 * @if Eng
 * @brief  Set status of slp radar.
 * @par Description: Set status of slp radar.
 * @param [in] status status of slp radar, see @ref radar_set_status_t.
 * @retval error code.
 * @else
 * @brief  设置雷达状态。
 * @par Description: 设置雷达状态。
 * @param [in] status 雷达状态。
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_set_status(uint8_t status);

/**
 * @if Eng
 * @brief  Set hardware parameter of slp radar.
 * @par Description: Set hardware parameter of slp radar.
 * @param [in] para hardware parameter of slp radar, see @ref slp_radar_hardware_para_t.
 * @retval error code.
 * @else
 * @brief  设置雷达硬件参数。
 * @par Description: 设置雷达硬件参数。
 * @param [in] para 雷达硬件参数。
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_set_hardware_para(slp_radar_hardware_para_t *para);

/**
 * @if Eng
 * @brief  Set algorithm parameter of slp radar.
 * @par Description: Set algorithm parameter of slp radar.
 * @param [in] para algorithm Parameter of slp radar, see @ref slp_radar_alg_para_t.
 * @retval error code.
 * @else
 * @brief  设置雷达算法参数。
 * @par Description: 设置雷达算法参数。
 * @param [in] para 雷达算法参数。
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_set_alg_para(slp_radar_alg_para_t *para);

/**
 * @if Eng
 * @brief  Radar result callback registration function.
 * @par Description: Radar result callback registration function.
 * @param [in] cb slp_radar_result_cb_t.
 * @retval error code.
 * @else
 * @brief  雷达检测结果回调注册函数。
 * @par Description: 雷达检测结果回调注册函数。
 * @param [in] cb 回调函数。
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_register_result_cb(slp_radar_result_cb_t cb);

/**
 * @if Eng
 * @brief  Get radar detection result.
 * @par Description: Get radar etection result.
 * @param [in] result: slp_radar_result_t.
 * @retval error code.
 * @else
 * @brief  获取雷达检测结果。
 * @par Description: 获取雷达检测结果。
 * @param [in] result 雷达检测结果。
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_get_result(slp_radar_result_t *result);

/**
 * @if Eng
 * @brief  Radar set status callback registration function.
 * @par Description: Radar set status callback registration function.
 * @param [in] cb slp_radar_set_status_cb_t.
 * @retval error code.
 * @else
 * @brief  雷达设置状态回调注册函数。
 * @par Description: 雷达设置状态回调注册函数。
 * @param [in] cb 回调函数。
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_register_set_status_cb(slp_radar_set_status_cb_t cb);

/**
 * @if Eng
 * @brief  Radar report errcode callback registration function.
 * @par Description: Radar report errcode callback registration function.
 * @param [in] cb slp_radar_report_errcode_cb_t.
 * @retval error code.
 * @else
 * @brief  雷达错误码回调注册函数。
 * @par Description: 雷达错误码回调注册函数。
 * @param [in] cb 回调函数。
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_register_report_errcode_cb(slp_radar_report_errcode_cb_t cb);

/**
 * @if Eng
 * @brief  Get slp radar version number.
 * @par Description: Get slp radar version number.
 * @param [in] version_num.
 * @retval error code.
 * @else
 * @brief  获取雷达版本号。
 * @par Description: 获取雷达版本号。
 * @param [in] version_num 版本号
 * @retval 执行结果错误码。
 * @endif
 */
errcode_slp_radar_client_t uapi_slp_radar_get_version_num(uint32_t *version_num);

#ifdef __cplusplus
}
#endif
#endif