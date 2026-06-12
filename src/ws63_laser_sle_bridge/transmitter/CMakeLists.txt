#===============================================================================
# @brief WS63 Laser SLE Bridge transmitter sources
#===============================================================================

set(SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_passthrough.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    PARENT_SCOPE)
