@echo off
setlocal EnableDelayedExpansion
title zluda_rx580 - установка ZLUDA

rem ============================================================
rem  Только ZLUDA: скачивание, распаковка, проверка.
rem  Не трогает ни venv, ни ComfyUI - их можно не переустанавливать.
rem
rem  Запуск:  install_zluda.bat [--quiet]
rem    --quiet  не показывать "Press any key" (для вызова из install.bat)
rem
rem  Всё кладётся строго в <каталог набора>\zluda\
rem ============================================================

cd /d "%~dp0"
call env.bat

set "ZLUDA_DIR=%~dp0zluda"
set "ZLUDA_REL=rel.854c58e1565c3597e17046d7cc2b1eab96fcdf55"
set "ZLUDA_URL=https://github.com/lshqqytiger/ZLUDA/releases/download/%ZLUDA_REL%/ZLUDA-windows-rocm5-amd64.zip"

set "CURL=%SystemRoot%\system32\curl.exe"
set "TAR=%SystemRoot%\system32\tar.exe"
if not exist "%CURL%" set "CURL=curl.exe"
if not exist "%TAR%" set "TAR=tar.exe"

echo.
echo ================================================================
echo   Установка ZLUDA 3.9.6 (rocm5 / HIP 5.7) в %ZLUDA_DIR%
echo ================================================================
echo.

if exist "%ZLUDA_DIR%\zluda.exe" (
    echo ZLUDA уже распакован, пропускаю скачивание.
    goto :verify
)

if not exist "%ZLUDA_DIR%" mkdir "%ZLUDA_DIR%"

set "ZIP=%ZLUDA_DIR%\zluda.zip"

rem --- архив: сначала локальный, иначе скачиваем ----------------
if exist "%ZIP%" (
    echo беру локальный архив %ZIP%
) else (
    echo скачиваю
    echo   %ZLUDA_URL%
    "%CURL%" -fL --retry 3 --retry-delay 3 --ssl-no-revoke --connect-timeout 30 -o "%ZIP%" "%ZLUDA_URL%"
    if errorlevel 1 (
        echo.
        echo [!] curl завершился с ошибкой ^(код !errorlevel!^).
        echo     Возможные причины: нет доступа к github.com, прокси, антивирус.
        if exist "%ZIP%" del "%ZIP%"
        goto :manual
    )
)

for %%S in ("%ZIP%") do set "ZSIZE=%%~zS"
echo размер архива: !ZSIZE! байт
if !ZSIZE! LSS 1000000 (
    echo.
    echo [!] Архив весит всего !ZSIZE! байт - это не архив ZLUDA,
    echo     а, скорее всего, страница с ошибкой. Удаляю.
    del "%ZIP%"
    goto :manual
)

rem --- распаковка ------------------------------------------------
echo распаковываю
pushd "%ZLUDA_DIR%"
"%TAR%" -xf zluda.zip
if errorlevel 1 (
    popd
    echo.
    echo [!] tar.exe не смог распаковать архив ^(код !errorlevel!^).
    goto :manual
)
popd

rem --- архив мог распаковаться в подпапку ------------------------
if not exist "%ZLUDA_DIR%\zluda.exe" (
    for /d %%D in ("%ZLUDA_DIR%\*") do (
        if exist "%%D\zluda.exe" (
            echo архив распакован в подпапку %%~nxD - переношу файлы на уровень выше
            move /y "%%D\*" "%ZLUDA_DIR%\" >nul
            rmdir "%%D"
        )
    )
)

:verify
echo.
echo --- содержимое %ZLUDA_DIR% ---
dir /b "%ZLUDA_DIR%" 2>nul
echo.

set "MISSING="
for %%F in (zluda.exe zluda.dll zluda_inject.exe nvcuda.dll nvml.dll) do (
    if not exist "%ZLUDA_DIR%\%%F" set "MISSING=!MISSING! %%F"
)

if exist "%ZLUDA_DIR%\zluda.exe" (
    echo [OK] zluda.exe на месте.
    if not "!MISSING!"=="" echo [!] Отсутствуют:!MISSING!
    echo.
    echo Дальше:
    echo   1. install_comfyui.bat   - подменит библиотеки в torch\lib
    echo   2. run_diagnose.bat      - проверка
    if /i not "%1"=="--quiet" pause
    exit /b 0
)

echo [!] zluda.exe так и не появился.
:manual
echo.
echo ================================================================
echo   Ручная установка
echo ================================================================
echo.
echo   1. Скачайте в браузере:
echo        %ZLUDA_URL%
echo   2. Сохраните как:
echo        %ZLUDA_DIR%\zluda.zip
echo   3. Запустите этот скрипт ещё раз - он распакует локальный архив.
echo.
echo   Либо распакуйте сами так, чтобы файл лежал здесь:
echo        %ZLUDA_DIR%\zluda.exe
echo.
echo   Проверка после установки:  dir "%ZLUDA_DIR%\zluda.exe"
echo ================================================================
echo.
if /i not "%1"=="--quiet" pause
exit /b 1
