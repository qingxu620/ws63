@echo off
chcp 65001 >nul 2>&1
title WS63 环境初始化

echo ================================================
echo   WS63 激光打标机 - 测试环境初始化
echo ================================================
echo.

REM === 检测 Python ===
where py >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set PYTHON_CMD=py -3
    echo [OK] 找到 Python: py -3
    goto :found_python
)

where python >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set PYTHON_CMD=python
    echo [OK] 找到 Python: python
    goto :found_python
)

echo [ERROR] 未找到 Python!
echo   请从 https://www.python.org/downloads/ 下载并安装 Python 3.8+
echo   安装时请勾选 "Add Python to PATH"
echo.
pause
exit /b 1

:found_python
%PYTHON_CMD% --version
echo.

REM === 安装 pyserial ===
echo 正在安装 pyserial ...
%PYTHON_CMD% -m pip install pyserial --quiet
if %ERRORLEVEL% neq 0 (
    echo [ERROR] pyserial 安装失败，请检查网络连接
    pause
    exit /b 1
)
echo [OK] pyserial 已安装
echo.

REM === 生成 test_config.json 模板 ===
set CONFIG_FILE=%~dp0test_config.json
if exist "%CONFIG_FILE%" (
    echo [SKIP] test_config.json 已存在，跳过生成
    echo   如需重新生成，请先删除该文件
) else (
    echo 正在生成 test_config.json 模板 ...
    (
        echo {
        echo   "business_port": "COM8",
        echo   "tx_debug_port": "COM11",
        echo   "rx_debug_port": "COM13",
        echo   "baud": 115200,
        echo   "debug_baud": 115200
        echo }
    ) > "%CONFIG_FILE%"
    echo [OK] 已生成 %CONFIG_FILE%
    echo.
    echo !! 重要 !!
    echo   请用记事本打开 test_config.json
    echo   将 COM 端口号修改为你的实际端口
    echo   可在 设备管理器 → 端口(COM和LPT) 中查看
)

echo.
echo ================================================
echo   环境初始化完成！
echo   下一步: 编辑 test_config.json 填入实际端口号
echo   然后运行: run_test.bat
echo ================================================
echo.
pause
