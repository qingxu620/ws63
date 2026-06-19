#===============================================================================
# @brief WS63 ST7796 LCD + FT6336 touch screen driver
#===============================================================================

set(SOURCES
    ${SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/screen_board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/st7796_lcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/st7796_text.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ft6336_touch.c
)

set(PUBLIC_HEADER
    ${PUBLIC_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
