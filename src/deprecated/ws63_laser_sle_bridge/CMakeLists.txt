#===============================================================================
# @brief WS63 laser SLE transparent bridge sample
#===============================================================================

if(DEFINED CONFIG_LASER_SLE_BRIDGE_TRANSMITTER)
    add_subdirectory_if_exist(transmitter)
elseif(DEFINED CONFIG_LASER_SLE_BRIDGE_RECEIVER)
    add_subdirectory_if_exist(receiver)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
