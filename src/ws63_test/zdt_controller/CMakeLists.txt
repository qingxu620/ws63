#===============================================================================
# @brief    WS63 ZDT Controller Board CMakeLists.txt
# @note     将 ZDT 控制板源文件加入编译
#===============================================================================

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Emm_V5.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/zdt_uart.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/zdt_protocol.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/zdt_controller.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    PARENT_SCOPE)
