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

set "VENV_DIR="
set "DARGS="
set "FLAG="
set "USE_ZLUDA=1"
rem Разбор аргументов: имя окружения может стоять только до первого флага.
rem Иначе "run_diagnose.bat --check patch" принял бы "patch" за имя окружения.
rem --no-zluda вырезается: diagnose.py про такой ключ ничего не знает.
for %%A in (%*) do (
    set "ARG=%%~A"
    if /i "!ARG!"=="--no-zluda" (
        set "USE_ZLUDA=0"
    ) else if not defined FLAG (
        if "!ARG:~0,2!"=="--" (
            set "FLAG=1"
            set "DARGS=!DARGS! !ARG!"
        ) else if not defined VENV_DIR (
            set "VENV_DIR=!ARG!"
        ) else (
            set "DARGS=!DARGS! !ARG!"
        )
    ) else (
        set "DARGS=!DARGS! !ARG!"
    )
)
if not defined VENV_DIR set "VENV_DIR=venv"

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
set "ZLUDA_STATE=%ZLUDA_EXE%"
if "%USE_ZLUDA%"=="0" set "ZLUDA_STATE=выключен ключом --no-zluda"
if "%USE_ZLUDA%"=="1" if not exist "%ZLUDA_EXE%" set "ZLUDA_STATE=НЕ НАЙДЕН %ZLUDA_EXE%"

echo.
echo *** Окружение: %VENV_DIR%
echo *** Python:    "%PYTHON%"
echo *** ZLUDA:     !ZLUDA_STATE!
if "%USE_ZLUDA%"=="1" if not exist "%ZLUDA_EXE%" (
    echo     [!] Файла zluda\zluda.exe нет. Без него CUDA не появится вовсе и
    echo         все проверки GPU дадут "Found no NVIDIA driver".
    echo         Проверьте каталог %KIT_DIR%\zluda и запустите install_zluda.bat -
    echo         он скачает и распакует ZLUDA, не трогая venv и ComfyUI.
)
if "%USE_ZLUDA%"=="0" echo     [!] Без ZLUDA проверки GPU упрутся в "Found no NVIDIA driver".
echo.

if "%USE_ZLUDA%"=="1" if exist "%ZLUDA_EXE%" (
    "%ZLUDA_EXE%" -- "%PYTHON%" -m zluda_rx580.diagnose %DARGS%
) else (
    "%PYTHON%" -m zluda_rx580.diagnose %DARGS%
)

echo.
pause
