#===============================================================================
# @brief    cmake file
# Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
#===============================================================================
if(DEFINED CONFIG_SUPPORT_RADAR_STA_SAMPLE)
    add_subdirectory_if_exist(sta_sample)
endif()

if(DEFINED CONFIG_SUPPORT_RADAR_SOFTAP_SAMPLE)
    add_subdirectory_if_exist(softap_sample)
endif()

if(DEFINED CONFIG_SUPPORT_RADAR_WIRELESS_SAMPLE)
    add_subdirectory_if_exist(wireless_sample)
endif()

if(DEFINED CONFIG_SUPPORT_RADAR_SLP_SAMPLE)
    add_subdirectory_if_exist(slp_sample)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
