@echo off
setlocal EnableDelayedExpansion
title zluda_rx580 - диагностика

rem ============================================================
rem  Диагностика ZLUDA + PyTorch.
rem  Запуск:  run_diagnose.bat [имя_окружения] [параметры]
rem  Примеры:
rem    run_diagnose.bat                        (окружение "venv")
rem    run_diagnose.bat venv-torch27           (другое окружение)
rem    run_diagnose.bat venv --check pin       (одна проверка)
rem    run_diagnose.bat venv --patch           (с подменой cudaHostAlloc)
rem    run_diagnose.bat venv --list            (список проверок)
rem
rem  Работает и на старом torch 2.2.1 - это способ подтвердить диагноз.
rem ============================================================

set "KIT_DIR=%~dp0"
if "%KIT_DIR:~-1%"=="\" set "KIT_DIR=%KIT_DIR:~0,-1%"
cd /d "%KIT_DIR%"

set "VENV_DIR=%~1"
if "%VENV_DIR%"=="" set "VENV_DIR=venv"
if /i "%VENV_DIR:~0,2%"=="--" set "VENV_DIR=venv"

if not exist "%VENV_DIR%\Scripts\python.exe" (
    echo.
    echo [!] Окружение "%VENV_DIR%" не найдено.
    echo     Запуск: run_diagnose.bat [имя_окружения] [параметры]
    echo.
    pause
    exit /b 1
)

set "PYTHON=%KIT_DIR%\%VENV_DIR%\Scripts\python.exe"
call "%KIT_DIR%\env.bat"

echo.
echo *** Окружение: %VENV_DIR%
echo *** Python:    "%PYTHON%"
echo.

if /i "%VENV_DIR%"=="venv" (
    "%PYTHON%" -m zluda_rx580.diagnose %*
) else (
    "%PYTHON%" -m zluda_rx580.diagnose %2 %3 %4 %5
)

echo.
pause
