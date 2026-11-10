@echo off
setlocal EnableDelayedExpansion
title ComfyUI - ZLUDA - RX 480/570/580

rem ============================================================
rem  Запуск ComfyUI через ZLUDA.
rem  Запуск:  run_comfyui.bat [имя_окружения]
rem
rem  ComfyUI ищется в подкаталоге ComfyUI\, а если его там нет -
rem  в текущем каталоге. Флаги добавляются только если они есть
rem  в comfy\cli_args.py, поэтому скрипт не ломается после
rem  обновления ComfyUI.
rem ============================================================

set "KIT_DIR=%~dp0"
if "%KIT_DIR:~-1%"=="\" set "KIT_DIR=%KIT_DIR:~0,-1%"
cd /d "%KIT_DIR%"

set "VENV_DIR=%~1"
if "%VENV_DIR%"=="" set "VENV_DIR=venv"
if /i "%VENV_DIR:~0,2%"=="--" set "VENV_DIR=venv"
if not exist "%VENV_DIR%\Scripts\python.exe" (
    echo [!] Окружение "%VENV_DIR%" не найдено.
    echo     Сначала запустите install_comfyui.bat
    pause
    exit /b 1
)

set "COMFY_DIR=ComfyUI"
if not exist "%COMFY_DIR%\main.py" set "COMFY_DIR=."
if not exist "%COMFY_DIR%\main.py" (
    echo [!] main.py не найден ни в ComfyUI\, ни в текущем каталоге.
    pause
    exit /b 1
)

set "PYTHON=%KIT_DIR%\%VENV_DIR%\Scripts\python.exe"
set "PYTHONPATH=%KIT_DIR%;%PYTHONPATH%"
set "PYTHONIOENCODING=utf-8"
set "ZLUDA_COMGR_LOG_LEVEL=1"

set "ARGS=--auto-launch --use-quad-cross-attention --reserve-vram 0.9"

for %%F in (disable-pinned-memory disable-async-offload disable-smart-memory) do (
    findstr /c:"--%%F" "%COMFY_DIR%\comfy\cli_args.py" >nul 2>&1
    if !errorlevel! equ 0 (
        set "ARGS=!ARGS! --%%F"
        echo  :: найден флаг --%%F
    ) else (
        echo  :: флага --%%F в этой версии ComfyUI нет - пропускаем
    )
)

echo.
echo *** Окружение: %VENV_DIR%
echo *** ComfyUI:   %COMFY_DIR%
echo *** Аргументы: !ARGS! %COMMANDLINE_ARGS%
echo.

cd /d "%KIT_DIR%\%COMFY_DIR%"

if exist "%KIT_DIR%\zluda\zluda.exe" (
    "%KIT_DIR%\zluda\zluda.exe" -- "%PYTHON%" main.py !ARGS! %COMMANDLINE_ARGS%
) else (
    echo [!] zluda\zluda.exe не найден - запуск без ZLUDA
    "%PYTHON%" main.py !ARGS! %COMMANDLINE_ARGS%
)

cd /d "%KIT_DIR%"
pause
