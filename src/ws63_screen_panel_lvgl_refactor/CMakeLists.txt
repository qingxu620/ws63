#===============================================================================
# @brief WS63 Screen Panel LVGL Refactor (minimal: LCD + touch + LVGL + demo UI)
#===============================================================================

set(LVGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_lvgl/src/lvgl)

add_definitions(-DLV_CONF_INCLUDE_SIMPLE)
add_definitions(-DSCREEN_PANEL_ENABLE_SD=1 -DSCREEN_SD_TEST_BITBANG_ONLY=0)

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)

set(SOURCES
    ${SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/spi_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/sd_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/fat_reader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/lcd_driver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/touch_driver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/board/screen_board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/board/ft6336_touch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/task_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/panel_model.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/panel_file_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/panel_transport_sle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/panel_job_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/panel_rx_commands.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/panel_offline_job.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/panel_theme.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ui_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/home_page.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/page_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/page_diagnostics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/page_job_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/page_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/page_file_browser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts/lv_font_panel_cn_14.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_panel_lvgl/src/ili9341_lcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ws63_laser_sle_tx/common/packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ws63_laser_sle_tx/common/crc16.c
    ${LVGL_SOURCES}
)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/board
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages
    ${CMAKE_CURRENT_SOURCE_DIR}/../ws63_laser_sle_tx/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_panel_lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_panel_lvgl/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_lvgl/src/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/../deprecated/ws63_screen_lvgl/src/lvgl/src
)

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
