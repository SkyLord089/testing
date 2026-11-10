@echo off
setlocal EnableDelayedExpansion
title Установка PyTorch 2.7 + ZLUDA (RX 480/570/580)

rem ============================================================
rem  Ставит PyTorch 2.7 (cu118) в ОТДЕЛЬНОЕ окружение venv-torch27.
rem  Существующее окружение с torch 2.2.1 не трогается.
rem ============================================================

set "VENV_DIR=venv-torch27"
set "TORCH_VER=2.7.1"
set "TVISION_VER=0.22.1"
set "TAUDIO_VER=2.7.1"
set "TORCH_INDEX=https://download.pytorch.org/whl/cu118"
set "ZLUDA_URL=https://github.com/lshqqytiger/ZLUDA/releases/download/rel.5e717459179dc272b7d7d23391f0fad66c7459cf/ZLUDA-windows-rocm5-amd64.zip"

echo.
echo  ::  1/6  Окружение %VENV_DIR%
if not exist "%VENV_DIR%\Scripts\activate.bat" python.exe -m venv %VENV_DIR%
if not exist "%VENV_DIR%\Scripts\activate.bat" (
    echo [!] Не удалось создать окружение. Проверьте, что python.exe есть в PATH.
    pause
    exit /b 1
)
call "%VENV_DIR%\Scripts\activate.bat"
python.exe -m pip install --upgrade pip --quiet

set "PYTHONPATH=%~dp0;%PYTHONPATH%"

echo  ::  2/6  PyTorch %TORCH_VER% +cu118 (файл около 2.7 ГБ)
pip install torch==%TORCH_VER% torchvision==%TVISION_VER% torchaudio==%TAUDIO_VER% --index-url %TORCH_INDEX% --quiet
if errorlevel 1 (
    echo  ::  %TORCH_VER% не установлен, пробуем 2.7.0
    set "TORCH_VER=2.7.0"
    set "TVISION_VER=0.22.0"
    set "TAUDIO_VER=2.7.0"
    pip install torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0 --index-url %TORCH_INDEX% --quiet
)
if errorlevel 1 (
    echo [!] Не удалось установить PyTorch. Проверьте сеть и наличие версий на %TORCH_INDEX%
    pause
    exit /b 1
)

echo  ::  3/6  Зависимости ComfyUI
if exist requirements.txt pip install -r requirements.txt --quiet

echo  ::  4/6  ZLUDA
if exist "zluda\zluda.exe" (
    echo      используем уже распакованный zluda\
) else (
    if not exist zluda mkdir zluda
    pushd zluda
    %SystemRoot%\system32\curl.exe -sL --ssl-no-revoke %ZLUDA_URL% > zluda.zip
    %SystemRoot%\system32\tar.exe -xf zluda.zip
    del zluda.zip
    popd
)

echo  ::  5/6  Подмена библиотек в torch\lib
set "TLIB=%VENV_DIR%\Lib\site-packages\torch\lib"
if exist "zluda\cublas.dll"    copy /y zluda\cublas.dll    "%TLIB%\cublas64_11.dll"    >nul
if exist "zluda\cusparse.dll"  copy /y zluda\cusparse.dll  "%TLIB%\cusparse64_11.dll"  >nul
if exist "%TLIB%\nvrtc64_112_0.dll" if not exist "%TLIB%\nvrtc_cuda.dll" copy /y "%TLIB%\nvrtc64_112_0.dll" "%TLIB%\nvrtc_cuda.dll" >nul
if exist "zluda\nvrtc.dll"     copy /y zluda\nvrtc.dll     "%TLIB%\nvrtc64_112_0.dll"  >nul
echo      cufft/cufftw НЕ подменяются: на HIP 5.7 их обычно нет в сборке ZLUDA rocm5

echo  ::  6/6  Автоактивация набора
python.exe -m zluda_rx580.install_pth

echo.
echo *** Готово. Теперь:
echo       run_diagnose.bat %VENV_DIR%      - проверка
echo       run_comfyui.bat  %VENV_DIR%      - запуск
echo.
pause
