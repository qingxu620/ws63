#===============================================================================
# @brief WS63 structured SLE laser TX sample
#===============================================================================

add_subdirectory_if_exist(common)

if(DEFINED CONFIG_LASER_SLE_JOB_TRANSMITTER)
    add_subdirectory_if_exist(transmitter)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
