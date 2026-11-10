@echo off
setlocal EnableDelayedExpansion
title zluda_rx580 - diagnostics

rem ============================================================
rem  Диагностика ZLUDA + PyTorch.
rem  Запуск:  run_diagnose.bat [путь_к_venv] [параметры]
rem  Примеры:
rem    run_diagnose.bat                       (venv в текущем каталоге)
rem    run_diagnose.bat venv-torch27          (другое окружение)
rem    run_diagnose.bat venv --check pin      (одна проверка)
rem    run_diagnose.bat venv --patch          (с подменой cudaHostAlloc)
rem  Работает и на старом torch 2.2.1 - это способ подтвердить диагноз.
rem ============================================================

set "VENV_DIR=%~1"
if "%VENV_DIR%"=="" set "VENV_DIR=venv"
if /i "%VENV_DIR:~0,6%"=="--" set "VENV_DIR=venv"

if not exist "%VENV_DIR%\Scripts\python.exe" (
    echo.
    echo [!] Окружение "%VENV_DIR%" не найдено.
    echo     Запуск: run_diagnose.bat [путь_к_venv] [параметры]
    echo.
    exit /b 1
)

set "PYTHON=%VENV_DIR%\Scripts\python.exe"
set "PYTHONPATH=%~dp0;%PYTHONPATH%"
set "PYTHONIOENCODING=utf-8"

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
