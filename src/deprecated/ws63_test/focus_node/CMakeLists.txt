#===============================================================================
# @brief    WS63 Focus Node CMakeLists.txt
# @note     感知与对焦节点第一版：TTL UART + ZDT Z轴
#===============================================================================

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/crc16.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/focus_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/focus_sle_server.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../zdt_controller/Emm_V5.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../zdt_controller/zdt_uart.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../zdt_controller/zdt_protocol.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../zdt_controller/zdt_controller.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../zdt_controller"
    PARENT_SCOPE)
