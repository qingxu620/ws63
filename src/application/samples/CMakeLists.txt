#===============================================================================
# @brief    cmake file
# Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
#===============================================================================
set(COMPONENT_NAME "samples")

set(SOURCES
)

set(PUBLIC_HEADER
)

if(DEFINED CONFIG_ENABLE_BT_SAMPLE)
    add_subdirectory_if_exist(bt)
endif()
if(DEFINED CONFIG_ENABLE_PERIPHERAL_SAMPLE)
    add_subdirectory_if_exist(peripheral)
endif()
if(DEFINED CONFIG_ENABLE_WIFI_SAMPLE)
    add_subdirectory_if_exist(wifi)
endif()
if(DEFINED CONFIG_ENABLE_PRODUCTS_SAMPLE)
    add_subdirectory_if_exist(products)
endif()
if(DEFINED CONFIG_ENABLE_RADAR_SAMPLE)
    add_subdirectory_if_exist(radar)
endif()
if(DEFINED CONFIG_ENABLE_NFC_SAMPLE)
    add_subdirectory_if_exist(nfc)
endif()

add_subdirectory_if_exist(custom)

# --------------------------------------------------------------------------
# Firmware variant counting: ensure exactly one main variant is selected.
# Only enforced for the main application build (ws63_liteos_app).
# Flashboot / loaderboot targets use their own .config and skip variants.
# --------------------------------------------------------------------------
if("${BUILD_TARGET_NAME}" STREQUAL "ws63_liteos_app")
    set(_variant_count 0)
    set(_variant_selected "")

    if(DEFINED CONFIG_ENABLE_LASER_SINGLE_SAMPLE)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "single")
    endif()
    if(DEFINED CONFIG_ENABLE_LASER_WIFI_SAMPLE)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "wifi")
    endif()
    if(DEFINED CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "tx")
    endif()
    if(DEFINED CONFIG_LASER_RX_UNIFIED)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "rx_unified")
    endif()
    if(DEFINED CONFIG_ENABLE_SCREEN_SAMPLE)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "screen_selftest")
    endif()
    if(DEFINED CONFIG_ENABLE_LVGL_SAMPLE)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "lvgl_sample")
    endif()
    if(DEFINED CONFIG_ENABLE_LVGL_PANEL)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "screen_panel")
    endif()
    if(DEFINED CONFIG_ENABLE_SD_CARD_TEST)
        math(EXPR _variant_count "${_variant_count} + 1")
        set(_variant_selected "sd_card_test")
    endif()

    if(NOT _variant_count EQUAL 1)
        message(FATAL_ERROR
            "Firmware variant count is ${_variant_count}, expected exactly 1. "
            "Selected: ${_variant_selected}. "
            "Check your .config for conflicting CONFIG_ENABLE_* settings.")
    endif()

    message(STATUS "Firmware variant selected: ${_variant_selected}")
endif()

if(DEFINED CONFIG_ENABLE_LASER_SINGLE_SAMPLE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../deprecated/ws63_laser_single ws63_laser_single)
elseif(DEFINED CONFIG_ENABLE_LASER_WIFI_SAMPLE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../deprecated/ws63_laser_wifi ws63_laser_wifi)
elseif(DEFINED CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../ws63_laser_sle_tx ws63_laser_sle_tx)
elseif(DEFINED CONFIG_LASER_RX_UNIFIED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../ws63_laser_rx_unified ws63_laser_rx_unified)
elseif(DEFINED CONFIG_ENABLE_SCREEN_SAMPLE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../deprecated/ws63_screen_st7796_ft6336 ws63_screen_st7796_ft6336)
elseif(DEFINED CONFIG_ENABLE_LVGL_SAMPLE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../deprecated/ws63_screen_panel_lvgl ws63_screen_panel_lvgl)
elseif(DEFINED CONFIG_ENABLE_LVGL_PANEL)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../ws63_screen_panel_lvgl_refactor ws63_screen_panel_lvgl_refactor)
elseif(DEFINED CONFIG_ENABLE_SD_CARD_TEST)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../deprecated/ws63_sd_card_test ws63_sd_card_test)
endif()



set(PRIVATE_HEADER
)

set(PRIVATE_DEFINES
)

set(PUBLIC_DEFINES
)

# use this when you want to add ccflags like -include xxx
set(COMPONENT_PUBLIC_CCFLAGS
)

set(COMPONENT_CCFLAGS
)

set(WHOLE_LINK
    true
)

set(MAIN_COMPONENT
    false
)

install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/peripheral" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/products" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/wifi/sta_sample" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/wifi/alilink_sample" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/wifi/softap_sample" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/wifi/ohos_connect" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/wifi/hilink_indie_upgrade" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/wifi/ble_wifi_cfg_sample" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/radar" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/nfc" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/bt/ble" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/bt/sle" "*")
install_sdk("${CMAKE_CURRENT_SOURCE_DIR}/bt/sle_chba" "*")

set(SOURCES "${SOURCES}" PARENT_SCOPE)

build_component()
