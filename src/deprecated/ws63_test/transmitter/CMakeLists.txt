#===============================================================================
# @brief    WS63 Laser Marker - Transmitter Board CMakeLists.txt
# @note     将发射板所有源文件加入编译
#===============================================================================

# 所有源文件 (公共 + 发射板)
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/crc16.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gcode_parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gcode_processor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_client.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/uart_handler.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/wifi_gcode_server.c"
    PARENT_SCOPE)

# 头文件搜索路径
set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    PARENT_SCOPE)
