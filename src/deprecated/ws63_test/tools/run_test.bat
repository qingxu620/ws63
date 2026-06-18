@echo off
chcp 65001 >nul 2>&1
title WS63 自动化测试

echo ================================================
echo   WS63 激光打标机 - 自动化验收测试
echo ================================================
echo.

REM === 检测 Python ===
where py >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set PYTHON_CMD=py -3
) else (
    where python >nul 2>&1
    if %ERRORLEVEL% equ 0 (
        set PYTHON_CMD=python
    ) else (
        echo [ERROR] 未找到 Python! 请先运行 setup_env.bat
        pause
        exit /b 1
    )
)

REM === 脚本路径 ===
set SCRIPT_DIR=%~dp0
set AUTO_TEST=%SCRIPT_DIR%uart_auto_test.py
set STRESS_TEST=%SCRIPT_DIR%stress_test.py
set TIMESTAMP=%date:~0,4%%date:~5,2%%date:~8,2%_%time:~0,2%%time:~3,2%%time:~6,2%
set TIMESTAMP=%TIMESTAMP: =0%

REM === 检查配置文件 ===
if not exist "%SCRIPT_DIR%test_config.json" (
    echo [WARN] test_config.json 不存在，将使用默认端口 COM8/COM11/COM13
    echo   建议先运行 setup_env.bat 生成配置文件
    echo.
)

REM === 检查脚本是否存在 ===
if not exist "%AUTO_TEST%" (
    echo [ERROR] 未找到 uart_auto_test.py
    echo   请确认脚本文件完整
    pause
    exit /b 1
)

echo 测试开始时间: %date% %time%
echo 日志目录: %SCRIPT_DIR%logs\
echo.

REM ============================================
echo [1/4] 运行 smoke 测试 ...
echo ============================================
%PYTHON_CMD% "%AUTO_TEST%" --suite smoke --log-dir "%SCRIPT_DIR%logs" --report-json "%SCRIPT_DIR%result_smoke_%TIMESTAMP%.json"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] smoke 测试失败! 请检查基本连接后重试
    echo   详细日志见 logs\ 目录
    echo.
    goto :summary
)
echo [OK] smoke 通过
echo.

REM ============================================
echo [2/4] 运行 square 测试 ...
echo ============================================
%PYTHON_CMD% "%AUTO_TEST%" --suite square --log-dir "%SCRIPT_DIR%logs" --report-json "%SCRIPT_DIR%result_square_%TIMESTAMP%.json"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] square 测试失败!
    goto :summary
)
echo [OK] square 通过
echo.

REM ============================================
echo [3/4] 运行 repeat 测试 (20 rounds) ...
echo ============================================
%PYTHON_CMD% "%AUTO_TEST%" --suite repeat --rounds 20 --log-dir "%SCRIPT_DIR%logs" --report-json "%SCRIPT_DIR%result_repeat_%TIMESTAMP%.json"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] repeat 测试失败!
    goto :summary
)
echo [OK] repeat 通过
echo.

REM ============================================
echo [4/4] 运行 stress 压力测试 (20 rounds x 50 cycles) ...
echo ============================================
if not exist "%STRESS_TEST%" (
    echo [SKIP] 未找到 stress_test.py，跳过压力测试
    goto :summary
)
%PYTHON_CMD% "%STRESS_TEST%" --suite repeat --rounds 20 --cycles 50 --log-dir "%SCRIPT_DIR%logs" --report-json "%SCRIPT_DIR%result_stress_%TIMESTAMP%.json"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] stress 压力测试失败!
    goto :summary
)
echo [OK] stress 通过
echo.

:summary
echo.
echo ================================================
echo   测试完成! 时间: %date% %time%
echo   JSON 报告: result_*_%TIMESTAMP%.json
echo   日志文件:  logs\
echo ================================================
echo.
pause
