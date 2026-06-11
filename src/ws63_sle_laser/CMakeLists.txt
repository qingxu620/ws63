#===============================================================================
# @brief WS63 SLE Laser Marker - minimal passthrough design
#===============================================================================

add_subdirectory_if_exist(transmitter)
add_subdirectory_if_exist(receiver)

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
