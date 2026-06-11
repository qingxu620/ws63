#===============================================================================
# @brief WS63 SLE Transmitter (Passthrough) sources
#===============================================================================

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_passthrough.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    PARENT_SCOPE)
