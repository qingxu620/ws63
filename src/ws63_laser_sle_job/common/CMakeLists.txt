#===============================================================================
# @brief WS63 structured SLE laser job common sources
#===============================================================================

set(SOURCES
    ${SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/packet.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/crc16.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    "${CMAKE_CURRENT_SOURCE_DIR}"
    PARENT_SCOPE)
