#===============================================================================
# @brief WS63 structured SLE laser job sample
#===============================================================================

add_subdirectory_if_exist(common)

if(DEFINED CONFIG_LASER_SLE_JOB_TRANSMITTER)
    add_subdirectory_if_exist(transmitter)
elseif(DEFINED CONFIG_LASER_SLE_JOB_RECEIVER)
    add_subdirectory_if_exist(receiver)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
