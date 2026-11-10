@echo off
setlocal EnableDelayedExpansion
title Установка ComfyUI + PyTorch 2.7 + ZLUDA (RX 480/570/580)

rem ============================================================
rem  Полная установка с нуля:
rem    1. проверка Python
rem    2. скачивание ComfyUI
rem    3. виртуальное окружение
rem    4. PyTorch 2.7 +cu118 и все зависимости ComfyUI
rem    5. ZLUDA и подмена библиотек
rem    6. автоактивация набора zluda_rx580
rem
rem  Запуск:  install_comfyui.bat [имя_окружения]
rem  По умолчанию окружение называется "venv".
rem
rem  Скрипт идемпотентен: повторный запуск обновит ComfyUI
rem  и доустановит недостающее, не ломая уже поставленное.
rem ============================================================

set "KIT_DIR=%~dp0"
if "%KIT_DIR:~-1%"=="\" set "KIT_DIR=%KIT_DIR:~0,-1%"
cd /d "%KIT_DIR%"

set "VENV_DIR=%~1"
if "%VENV_DIR%"=="" set "VENV_DIR=venv"

rem Если набор скопирован внутрь уже существующего ComfyUI - используем его.
set "COMFY_DIR=ComfyUI"
if exist "main.py" set "COMFY_DIR=."
set "COMFY_REPO=https://github.com/comfyanonymous/ComfyUI.git"
set "COMFY_ZIP=https://codeload.github.com/comfyanonymous/ComfyUI/zip/refs/heads/master"
set "CURL=%SystemRoot%\system32\curl.exe"
set "TAR=%SystemRoot%\system32\tar.exe"

echo.
echo ================================================================
echo   ComfyUI + ZLUDA для AMD RX 480 / 570 / 580
echo   каталог: %KIT_DIR%
echo   окружение: %VENV_DIR%
echo ================================================================
echo.

rem ------------------------------------------------------------
rem  1/6  Проверка Python
rem ------------------------------------------------------------
echo  ::  1/6  Проверка Python
where python.exe >nul 2>&1
if errorlevel 1 (
    echo.
    echo [!] python.exe не найден в PATH.
    echo     Установите Python 3.11 или 3.12 с https://www.python.org/downloads/windows/
    echo     При установке ОБЯЗАТЕЛЬНО отметьте "Add python.exe to PATH".
    echo.
    pause
    exit /b 1
)

for /f "delims=" %%V in ('python.exe -c "import sys;print('.'.join(map(str, sys.version_info[:3])))"') do set "PYVER=%%V"
echo      Python %PYVER%

python.exe -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 (
    echo [!] ComfyUI требует Python 3.10 или новее ^(pyproject.toml: requires-python = ">=3.10"^).
    pause
    exit /b 1
)

python.exe -c "import sys; sys.exit(0 if (3, 10) <= sys.version_info < (3, 13) else 1)"
if errorlevel 1 (
    echo  ::  [?] Python %PYVER% вне проверенного диапазона 3.10-3.12.
    echo      ComfyUI заработает, но колесо PyTorch 2.7 +cu118 для этой версии
    echo      может отсутствовать. Рекомендуемые версии - 3.11 и 3.12.
)

if not exist "%CURL%" set "CURL=curl.exe"
if not exist "%TAR%" set "TAR=tar.exe"

rem ------------------------------------------------------------
rem  2/6  ComfyUI
rem ------------------------------------------------------------
echo.
echo  ::  2/6  ComfyUI
if "%COMFY_DIR%"=="." (
    echo      набор лежит внутри существующего ComfyUI - использую его, не обновляю
) else if exist "%COMFY_DIR%\main.py" (
    echo      уже есть в %COMFY_DIR%\ - обновляю
    if exist "%COMFY_DIR%\.git" (
        pushd "%COMFY_DIR%"
        git pull --ff-only --quiet
        if errorlevel 1 echo      [!] git pull не удался - остаюсь на текущей версии
        popd
    )
) else (
    where git.exe >nul 2>&1
    if not errorlevel 1 (
        echo      клонирую %COMFY_REPO%
        git clone --depth 1 "%COMFY_REPO%" "%COMFY_DIR%"
        if errorlevel 1 (
            echo [!] git clone не удался
            pause
            exit /b 1
        )
    ) else (
        echo      git не найден - скачиваю архив
        "%CURL%" -sL --ssl-no-revoke "%COMFY_ZIP%" > ComfyUI-master.zip
        if not exist ComfyUI-master.zip (
            echo [!] Не удалось скачать ComfyUI
            pause
            exit /b 1
        )
        "%TAR%" -xf ComfyUI-master.zip
        del ComfyUI-master.zip
        if exist "%COMFY_DIR%" rmdir /S /Q "%COMFY_DIR%"
        ren ComfyUI-master "%COMFY_DIR%"
    )
)
if not exist "%COMFY_DIR%\main.py" (
    echo [!] %COMFY_DIR%\main.py не найден - установка прервана
    pause
    exit /b 1
)
for /f "delims=" %%V in ('findstr /r "__version__" "%COMFY_DIR%\comfyui_version.py"') do echo      %%V

rem ------------------------------------------------------------
rem  3/6 - 6/6  Окружение, PyTorch, ZLUDA
rem ------------------------------------------------------------
echo.
echo  ::  3/6  Виртуальное окружение %VENV_DIR%
if not exist "%VENV_DIR%\Scripts\activate.bat" (
    python.exe -m venv %VENV_DIR%
    if errorlevel 1 (
        echo [!] Не удалось создать окружение
        pause
        exit /b 1
    )
)
call "%VENV_DIR%\Scripts\activate.bat"
python.exe -m pip install --upgrade pip --quiet

set "PYTHONPATH=%KIT_DIR%;%PYTHONPATH%"
set "PYTHONIOENCODING=utf-8"
set "TORCH_INDEX=https://download.pytorch.org/whl/cu118"

echo.
echo  ::  4/6  PyTorch 2.7 +cu118
set "TORCH_VER=2.7.1"
set "TVISION_VER=0.22.1"
set "TAUDIO_VER=2.7.1"
pip install torch==%TORCH_VER% torchvision==%TVISION_VER% torchaudio==%TAUDIO_VER% --index-url %TORCH_INDEX% --quiet
if errorlevel 1 (
    echo      %TORCH_VER% не установлен - пробую 2.7.0
    set "TORCH_VER=2.7.0"
    set "TVISION_VER=0.22.0"
    set "TAUDIO_VER=2.7.0"
    pip install torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0 --index-url %TORCH_INDEX% --quiet
)
if errorlevel 1 (
    echo [!] Не удалось установить PyTorch с %TORCH_INDEX%
    echo     Проверьте сеть и наличие версии %TORCH_VER% для Python %PYVER%.
    pause
    exit /b 1
)

echo  ::  4/6  Зависимости ComfyUI ^(requirements.txt^)
pip install -r "%COMFY_DIR%\requirements.txt" --quiet
if errorlevel 1 echo      [!] часть пакетов не установилась - смотрите вывод pip выше

rem requirements.txt содержит "torch" без версии, поэтому проверяем,
rem что pip не подменил cu118-сборку на обычную из PyPI.
for /f "delims=" %%T in ('python.exe -c "import torch;print(torch.__version__)"') do set "TORCH_NOW=%%T"
echo      установлено: torch !TORCH_NOW!
echo !TORCH_NOW! | findstr "cu118" >nul
if errorlevel 1 (
    echo      [!] torch подменён на !TORCH_NOW! - возвращаю сборку cu118
    pip install --force-reinstall torch==%TORCH_VER% torchvision==%TVISION_VER% torchaudio==%TAUDIO_VER% --index-url %TORCH_INDEX% --quiet
)

echo.
echo  ::  5/6  ZLUDA
set "ZLUDA_URL=https://github.com/lshqqytiger/ZLUDA/releases/download/rel.5e717459179dc272b7d7d23391f0fad66c7459cf/ZLUDA-windows-rocm5-amd64.zip"
if exist "zluda\zluda.exe" (
    echo      использую уже распакованный zluda\
) else (
    if not exist zluda mkdir zluda
    pushd zluda
    "%CURL%" -sL --ssl-no-revoke "%ZLUDA_URL%" > zluda.zip
    "%TAR%" -xf zluda.zip
    del zluda.zip
    popd
)
if not exist "zluda\zluda.exe" echo      [!] zluda\zluda.exe не появился - проверьте сеть

set "TLIB=%VENV_DIR%\Lib\site-packages\torch\lib"
echo      подмена библиотек в %TLIB%
if exist "zluda\cublas.dll"               copy /y zluda\cublas.dll   "%TLIB%\cublas64_11.dll"   >nul
if exist "zluda\cusparse.dll"             copy /y zluda\cusparse.dll "%TLIB%\cusparse64_11.dll" >nul
if exist "%TLIB%\nvrtc64_112_0.dll" if not exist "%TLIB%\nvrtc_cuda.dll" copy /y "%TLIB%\nvrtc64_112_0.dll" "%TLIB%\nvrtc_cuda.dll" >nul
if exist "zluda\nvrtc.dll"                copy /y zluda\nvrtc.dll    "%TLIB%\nvrtc64_112_0.dll" >nul
echo      cufft/cufftw не подменяются: в сборке ZLUDA rocm5 их обычно нет

echo.
echo  ::  6/6  Автоактивация zluda_rx580
python.exe -m zluda_rx580.install_pth

echo.
echo ================================================================
echo   Готово.
echo.
echo     1. Положите модели в %COMFY_DIR%\models\checkpoints
echo     2. Проверьте установку:   run_diagnose.bat %VENV_DIR%
echo     3. Запустите:             run_comfyui.bat %VENV_DIR%
echo.
echo   Первый запуск компилирует ядра ZLUDA - это может занять
echo   от 10 до 40 минут. Это нормально, не прерывайте.
echo ================================================================
echo.
pause
