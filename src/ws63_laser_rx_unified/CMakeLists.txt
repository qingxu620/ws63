#===============================================================================
# @brief WS63 unified laser RX skeleton
#===============================================================================

set(SOURCES
    ${SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rx_core/rx_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rx_core/rx_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rx_core/rx_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rx_core/rx_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/transports/uart_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/dac8562.c
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/laser_ctrl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/executor/gcode_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/executor/gcode_processor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/executor/motion_executor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/safety/safety.c
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/diag_log.c
    PARENT_SCOPE)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/rx_core
    ${CMAKE_CURRENT_SOURCE_DIR}/transports
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware
    ${CMAKE_CURRENT_SOURCE_DIR}/executor
    ${CMAKE_CURRENT_SOURCE_DIR}/safety
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    PARENT_SCOPE)
