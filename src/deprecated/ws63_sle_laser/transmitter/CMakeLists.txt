#===============================================================================
# @brief WS63 SLE Transmitter (Passthrough) sources
#===============================================================================

set(SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_passthrough.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/crc16.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../receiver/gcode_parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../receiver/gcode_processor.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../receiver"
    PARENT_SCOPE)
