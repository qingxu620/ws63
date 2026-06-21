#===============================================================================
# @brief WS63 unified laser RX skeleton
#===============================================================================

set(RX_UNIFIED_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mode_select/route_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mode_select/route_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/dac8562.c
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/laser_ctrl.c)

set(RX_UNIFIED_PUBLIC_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/mode_select
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware
    ${CMAKE_CURRENT_SOURCE_DIR}/routes
    ${CMAKE_CURRENT_SOURCE_DIR}/shared_safety)

if(DEFINED CONFIG_LASER_RX_TRANSPORT_UART)
    list(APPEND RX_UNIFIED_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_uart/legacy_uart_route.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_uart/legacy_uart_gcode_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_uart/legacy_uart_gcode_processor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_uart/legacy_uart_motion_executor.c)

    list(APPEND RX_UNIFIED_PUBLIC_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_uart)
endif()

if(DEFINED CONFIG_LASER_RX_TRANSPORT_WIFI)
    list(APPEND RX_UNIFIED_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_wifi/legacy_wifi_route.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_wifi/legacy_wifi_gcode_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_wifi/legacy_wifi_gcode_processor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_wifi/legacy_wifi_motion_executor.c)

    list(APPEND RX_UNIFIED_PUBLIC_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/legacy_wifi)
endif()

if(DEFINED CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
    list(APPEND RX_UNIFIED_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_route.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_route_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_manager.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_cache.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_packet.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_crc16.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_gcode_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_gcode_processor.c
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job/sle_job_motion_executor.c)

    list(APPEND RX_UNIFIED_PUBLIC_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/routes/sle_job)
endif()

set(SOURCES
    ${SOURCES}
    ${RX_UNIFIED_SOURCES}
    PARENT_SCOPE)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    ${RX_UNIFIED_PUBLIC_HEADER}
    PARENT_SCOPE)
