#===============================================================================
# @brief WS63 MSP3223 SD-card isolation test
#===============================================================================

set(SOURCES
    ${SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ws63_screen_panel_lvgl_refactor/src/hal/spi_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ws63_screen_panel_lvgl_refactor/src/hal/sd_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ws63_screen_panel_lvgl_refactor/src/hal/fat_reader.c
)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ws63_screen_panel_lvgl_refactor/src/hal
)

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
