#===============================================================================
# @brief WS63 structured SLE laser job receiver
#===============================================================================

set(SOURCES
    ${SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_job_server.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/job_cache.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/job_manager.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gcode_parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gcode_processor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/motion_executor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dac8562.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/laser_ctrl.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    PARENT_SCOPE)
