@echo off
setlocal EnableDelayedExpansion
title ComfyUI - ZLUDA - RX 480/570/580

rem ============================================================
rem  Запуск ComfyUI через ZLUDA.
rem  Флаги добавляются только если они есть в comfy/cli_args.py -
rem  скрипт не сломается после обновления ComfyUI.
rem ============================================================

set "VENV_DIR=%~1"
if "%VENV_DIR%"=="" set "VENV_DIR=venv-torch27"
if not exist "%VENV_DIR%\Scripts\python.exe" set "VENV_DIR=venv"
if not exist "%VENV_DIR%\Scripts\python.exe" (
    echo [!] Не найдено ни venv-torch27, ни venv. Сначала запустите install_pytorch27.bat
    pause
    exit /b 1
)

set "PYTHON=%VENV_DIR%\Scripts\python.exe"
set "PYTHONPATH=%~dp0;%PYTHONPATH%"
set "PYTHONIOENCODING=utf-8"
set "ZLUDA_COMGR_LOG_LEVEL=1"

set "ARGS=--auto-launch --use-quad-cross-attention --reserve-vram 0.9"

rem --reserve-vram: на 8 ГБ оставлять 0.9 ГБ под систему
rem --disable-pinned-memory: ComfyUI сам зовёт cudaHostRegister (model_management.py:1701)
rem --disable-async-offload: асинхронная выгрузка весов ломается на старых GPU

for %%F in (disable-pinned-memory disable-async-offload disable-smart-memory) do (
    findstr /c:"--%%F" comfy\cli_args.py >nul 2>&1
    if !errorlevel! equ 0 (
        set "ARGS=!ARGS! --%%F"
        echo  :: найден флаг --%%F
    ) else (
        echo  :: флаг --%%F отсутствует в этой версии ComfyUI - пропускаем
    )
)

echo.
echo *** Окружение: %VENV_DIR%
echo *** Аргументы: !ARGS! %COMMANDLINE_ARGS%
echo.

if exist "zluda\zluda.exe" (
    zluda\zluda.exe -- "%PYTHON%" main.py !ARGS! %COMMANDLINE_ARGS%
) else (
    echo [!] zluda\zluda.exe не найден - запуск без ZLUDA
    "%PYTHON%" main.py !ARGS! %COMMANDLINE_ARGS%
)

pause
