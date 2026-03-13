#===============================================================================
# @brief    User samples cmake file
#===============================================================================

# 模仿官方写法：如果定义了 LED 的配置宏，则进入 led 文件夹
if(DEFINED CONFIG_SAMPLE_SUPPORT_MY_LED)
    add_subdirectory_if_exist(led)
endif()

# 模仿官方写法：如果定义了 PWM 的配置宏，则进入 pwm 文件夹
if(DEFINED CONFIG_SAMPLE_SUPPORT_MY_PWM)
    add_subdirectory_if_exist(pwm)
endif()

# 模仿官方写法：如果定义了 BUTTON 的配置宏，则进入 button 文件夹
if(DEFINED CONFIG_SAMPLE_SUPPORT_MY_BUTTON)
    add_subdirectory_if_exist(button)
endif()

# 模仿官方写法：如果定义了 USART 的配置宏，则进入 usart 文件夹
if(DEFINED CONFIG_SAMPLE_SUPPORT_MY_USART)
    add_subdirectory_if_exist(usart)
endif()

# --- 激光打标机 接收板 ---
if(DEFINED CONFIG_LASER_MARKER_RECEIVER)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../ws63_test/receiver/CMakeLists.txt)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../ws63_test/receiver receiver)
    endif()
endif()

# --- 激光打标机 发射板 ---
if(DEFINED CONFIG_LASER_MARKER_TRANSMITTER)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../ws63_test/transmitter/CMakeLists.txt)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../ws63_test/transmitter transmitter)
    endif()
endif()

# 以后如果你增加了 oled，就在这里继续添加：
# if(DEFINED CONFIG_SAMPLE_SUPPORT_MY_OLED)
#     add_subdirectory_if_exist(oled)
# endif()

# 关键：将子目录传回来的 SOURCES 和 PUBLIC_HEADER 继续向上传给 samples 目录
set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)