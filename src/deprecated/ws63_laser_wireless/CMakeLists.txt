#===============================================================================
# @brief WS63 wireless laser marker sample
#===============================================================================

if(DEFINED CONFIG_LASER_WIRELESS_RECEIVER)
    add_subdirectory_if_exist(receiver)
endif()

if(DEFINED CONFIG_LASER_WIRELESS_TRANSMITTER)
    add_subdirectory_if_exist(transmitter)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
