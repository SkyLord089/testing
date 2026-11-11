@echo off
setlocal EnableDelayedExpansion
title Установка в D:\Apps\Rx-580-ComfyUI

rem ============================================================
rem  Точка входа. Всё ставится СТРОГО в одну папку:
rem
rem      D:\Apps\Rx-580-ComfyUI
rem
rem  Запуск:
rem      install.bat                        - в D:\Apps\Rx-580-ComfyUI
rem      install.bat E:\Games\Comfy         - в другую папку
rem      install.bat D:\Apps\Rx-580-ComfyUI venv2   - и другое окружение
rem
rem  Скрипт копирует набор в целевую папку и запускает там
rem  install_comfyui.bat. Повторный запуск безопасен: уже
rem  установленные venv\, ComfyUI\, zluda\ и cache\ не трогаются.
rem ============================================================

set "TARGET=D:\Apps\Rx-580-ComfyUI"
if not "%~1"=="" set "TARGET=%~1"
if "%TARGET:~-1%"=="\" set "TARGET=%TARGET:~0,-1%"

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"

echo.
echo ================================================================
echo   ComfyUI + PyTorch 2.7 + ZLUDA для AMD RX 480 / 570 / 580
echo.
echo   источник:   %SRC%
echo   установка:  %TARGET%
echo ================================================================
echo.

if /i "%SRC%"=="%TARGET%" (
    echo  ::  набор уже лежит в целевой папке - копировать нечего
) else (
    if not exist "%TARGET%" (
        mkdir "%TARGET%"
        if errorlevel 1 (
            echo.
            echo [!] Не удалось создать "%TARGET%".
            echo     Проверьте, что диск существует и есть права на запись.
            echo     Другую папку можно задать так:  install.bat E:\путь
            echo.
            pause
            exit /b 1
        )
    )

    rem Список того, что копировать НЕ надо: уже установленное и служебное.
    set "EXCL=%TEMP%\zlrx_exclude.txt"
    >  "%EXCL%" echo \venv\
    >> "%EXCL%" echo \ComfyUI\
    >> "%EXCL%" echo \zluda\
    >> "%EXCL%" echo \cache\
    >> "%EXCL%" echo \tmp\
    >> "%EXCL%" echo \.git\
    >> "%EXCL%" echo \__pycache__\

    echo  ::  Копирую набор в %TARGET%
    xcopy /E /I /Y /Q /EXCLUDE:%EXCL% "%SRC%" "%TARGET%\" >nul
    if errorlevel 1 echo      [!] часть файлов не скопировалась
    del "%EXCL%" >nul 2>&1
)

echo  ::  Запускаю установку в %TARGET%
echo.
cd /d "%TARGET%"
call "%TARGET%\install_comfyui.bat" %2

echo.
echo ================================================================
echo   Всё установлено в %TARGET%
echo.
echo     run_diagnose.bat   - проверка
echo     run_comfyui.bat    - запуск
echo.
echo   Запускать их нужно из этой же папки.
echo ================================================================
echo.
pause
