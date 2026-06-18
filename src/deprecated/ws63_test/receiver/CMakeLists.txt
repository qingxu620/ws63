#===============================================================================
# @brief    WS63 Laser Marker - Receiver Board CMakeLists.txt
# @note     将接收板所有源文件加入编译
#===============================================================================

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/crc16.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dac8562.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmd_queue.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/interpolator.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/laser_ctrl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/safety_monitor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_server.c"
    PARENT_SCOPE)

# 头文件搜索路径
set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    PARENT_SCOPE)
