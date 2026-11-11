@echo off
setlocal EnableDelayedExpansion
title zluda_rx580 - диагностика

rem ============================================================
rem  Диагностика ZLUDA + PyTorch.
rem  Запуск:  run_diagnose.bat [имя_окружения] [параметры]
rem  Примеры:
rem    run_diagnose.bat                        (окружение "venv")
rem    run_diagnose.bat venv-torch27           (другое окружение)
rem    run_diagnose.bat --check pin            (одна проверка)
rem    run_diagnose.bat --patch                (с подменой cudaHostAlloc)
rem    run_diagnose.bat --list                 (список проверок)
rem    run_diagnose.bat --no-zluda             (без zluda.exe, для сравнения)
rem
rem  ВАЖНО: проверки запускаются ЧЕРЕЗ zluda.exe. Без него CUDA не
rem  появится вовсе и вы получите "Found no NVIDIA driver" - это не
rem  поломка, это отсутствие ZLUDA. Дочерние процессы наследуют
rem  инъекцию: zluda_redirect перехватывает CreateProcessW.
rem ============================================================

set "KIT_DIR=%~dp0"
if "%KIT_DIR:~-1%"=="\" set "KIT_DIR=%KIT_DIR:~0,-1%"
cd /d "%KIT_DIR%"

set "VENV_DIR=%~1"
set "DARGS=%*"
if "%VENV_DIR%"=="" set "VENV_DIR=venv"
if /i not "%VENV_DIR:~0,2%"=="--" (
    rem первый аргумент - имя окружения, вырезаем его из списка параметров
    set "DARGS=%2 %3 %4 %5"
) else (
    set "VENV_DIR=venv"
)

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

set "ZLUDA_EXE=%KIT_DIR%\zluda\zluda.exe"
set "USE_ZLUDA=1"
for %%A in (%*) do if /i "%%A"=="--no-zluda" set "USE_ZLUDA=0"

echo.
echo *** Окружение: %VENV_DIR%
echo *** Python:    "%PYTHON%"
if "%USE_ZLUDA%"=="1" if exist "%ZLUDA_EXE%" (
    echo *** ZLUDA:     "%ZLUDA_EXE%"
) else (
    echo *** ZLUDA:     ВЫКЛЮЧЕН
    echo     [!] Без ZLUDA проверки GPU упрутся в "Found no NVIDIA driver".
)
echo.

if "%USE_ZLUDA%"=="1" if exist "%ZLUDA_EXE%" (
    "%ZLUDA_EXE%" -- "%PYTHON%" -m zluda_rx580.diagnose %DARGS%
) else (
    "%PYTHON%" -m zluda_rx580.diagnose %DARGS%
)

echo.
pause
