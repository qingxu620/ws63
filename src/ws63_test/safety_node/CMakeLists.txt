#===============================================================================
# @brief    WS63 Safety Node CMakeLists.txt
# @note     安全终端节点第一版：SLE + LED_ON / LED_OFF
#===============================================================================

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/crc16.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/safety_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/safety_sle_server.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    PARENT_SCOPE)
