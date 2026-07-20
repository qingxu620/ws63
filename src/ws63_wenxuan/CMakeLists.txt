#===============================================================================
# @brief WS63 Wenxuan phone-enabled unified laser RX variant
#===============================================================================

set(RX_UNIFIED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ws63_laser_rx_unified)

set(WENXUAN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${RX_UNIFIED_DIR}/mode_select/route_manager.c
    ${RX_UNIFIED_DIR}/mode_select/route_status.c
    ${RX_UNIFIED_DIR}/hardware/dac8563.c
    ${RX_UNIFIED_DIR}/hardware/laser_ctrl.c)

set(WENXUAN_PUBLIC_HEADER
    ${RX_UNIFIED_DIR}
    ${RX_UNIFIED_DIR}/common
    ${RX_UNIFIED_DIR}/mode_select
    ${RX_UNIFIED_DIR}/hardware
    ${RX_UNIFIED_DIR}/routes
    ${RX_UNIFIED_DIR}/shared_safety)

if(DEFINED CONFIG_LASER_RX_TRANSPORT_UART)
    list(APPEND WENXUAN_SOURCES
        ${RX_UNIFIED_DIR}/routes/legacy_uart/legacy_uart_route.c
        ${RX_UNIFIED_DIR}/routes/legacy_uart/legacy_uart_gcode_parser.c
        ${RX_UNIFIED_DIR}/routes/legacy_uart/legacy_uart_gcode_processor.c
        ${RX_UNIFIED_DIR}/routes/legacy_uart/legacy_uart_motion_executor.c)

    list(APPEND WENXUAN_PUBLIC_HEADER
        ${RX_UNIFIED_DIR}/routes/legacy_uart)
endif()

if(DEFINED CONFIG_LASER_RX_TRANSPORT_WIFI)
    list(APPEND WENXUAN_SOURCES
        ${RX_UNIFIED_DIR}/routes/legacy_wifi/legacy_wifi_route.c
        ${RX_UNIFIED_DIR}/routes/legacy_wifi/legacy_wifi_gcode_parser.c
        ${RX_UNIFIED_DIR}/routes/legacy_wifi/legacy_wifi_gcode_processor.c
        ${RX_UNIFIED_DIR}/routes/legacy_wifi/legacy_wifi_motion_executor.c)

    list(APPEND WENXUAN_PUBLIC_HEADER
        ${RX_UNIFIED_DIR}/routes/legacy_wifi)
endif()

if(DEFINED CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
    list(APPEND WENXUAN_SOURCES
        ${RX_UNIFIED_DIR}/routes/sle_job/sle_job_route.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_route_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_manager.c
        ${RX_UNIFIED_DIR}/routes/sle_job/sle_job_cache.c
        ${RX_UNIFIED_DIR}/routes/sle_job/sle_job_packet.c
        ${RX_UNIFIED_DIR}/routes/sle_job/sle_job_crc16.c
        ${RX_UNIFIED_DIR}/routes/sle_job/sle_job_gcode_parser.c
        ${RX_UNIFIED_DIR}/routes/sle_job/sle_job_gcode_processor.c
        ${RX_UNIFIED_DIR}/routes/sle_job/sle_job_motion_executor.c)

    list(APPEND WENXUAN_PUBLIC_HEADER
        ${RX_UNIFIED_DIR}/routes/sle_job)
endif()

if(DEFINED CONFIG_LASER_RX_TRANSPORT_SLE_JOB AND
   DEFINED CONFIG_LASER_RX_TRANSPORT_WIFI)
    list(APPEND WENXUAN_SOURCES
        ${RX_UNIFIED_DIR}/mode_select/boot_policy.c)
endif()

set(SOURCES
    ${SOURCES}
    ${WENXUAN_SOURCES}
    PARENT_SCOPE)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    ${WENXUAN_PUBLIC_HEADER}
    PARENT_SCOPE)
