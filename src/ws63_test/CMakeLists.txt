#===============================================================================
# @brief    WS63 Laser Marker - 顶层 CMakeLists.txt
# @note     根据 Kconfig 选择编译 接收板 或 发射板
#===============================================================================

if(DEFINED CONFIG_LASER_MARKER_RECEIVER)
    add_subdirectory_if_exist(receiver)
endif()

if(DEFINED CONFIG_LASER_MARKER_TRANSMITTER)
    add_subdirectory_if_exist(transmitter)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
