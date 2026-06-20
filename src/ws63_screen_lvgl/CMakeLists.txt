#===============================================================================
# @brief WS63 LVGL minimal port (MSP3223 ILI9341 + FT6336 + LVGL v9.3.0)
#===============================================================================

set(LVGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/lvgl)

# Ensure lv_conf.h is found by LVGL headers
add_definitions(-DLV_CONF_INCLUDE_SIMPLE)

# Collect all LVGL source files
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)

set(SOURCES
    ${SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lv_port_disp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lv_port_indev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lv_demo_panel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ili9341_lcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_st7796_ft6336/src/screen_board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_st7796_ft6336/src/ft6336_touch.c
    ${LVGL_SOURCES}
)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_st7796_ft6336/src
)

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
