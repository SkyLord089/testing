@echo off
rem Двойной щелчок по этому файлу = собрать gmouse.exe
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
pause
