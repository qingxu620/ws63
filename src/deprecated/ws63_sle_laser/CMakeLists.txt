#===============================================================================
# @brief WS63 SLE Laser Marker - minimal passthrough design
#===============================================================================

if(DEFINED CONFIG_SLE_LASER_TRANSMITTER)
    add_subdirectory_if_exist(transmitter)
elseif(DEFINED CONFIG_SLE_LASER_RECEIVER)
    add_subdirectory_if_exist(receiver)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
