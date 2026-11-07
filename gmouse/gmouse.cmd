@echo off
rem Обёртка: собирает gmouse при первом запуске и передаёт команду.
rem
rem   gmouse.cmd list
rem   gmouse.cmd test
rem   gmouse.cmd bind x1 double_click
rem   gmouse.cmd run
rem
rem Сборка идёт один раз; дальше запускается готовый exe.

setlocal
cd /d "%~dp0"

set "EXE=target\release\gmouse.exe"

if not exist "%EXE%" (
    echo Первый запуск: собираю gmouse.exe, это займёт до минуты...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
    if errorlevel 1 (
        echo.
        echo Сборка не удалась. Скачайте готовый gmouse.exe:
        echo     https://github.com/SkyLord089/testing/releases/latest
        exit /b 1
    )
)

if not exist "%EXE%" (
    echo gmouse.exe не найден после сборки.
    exit /b 1
)

"%EXE%" %*
exit /b %ERRORLEVEL%
