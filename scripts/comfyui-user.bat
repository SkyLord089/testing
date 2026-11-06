@echo off
rem ===========================================================================
rem  ComfyUI-Zluda launcher для RX 580 (gfx803) + HIP SDK 6.4 + torch 2.7+cu118
rem  Кладётся в корень ComfyUI-Zluda (рядом с main.py и venv\).
rem  Правьте только этот файл — штатные comfyui.bat/comfyui-n.bat перезапишутся
rem  при обновлении репозитория.
rem ===========================================================================
setlocal EnableDelayedExpansion

cd /d "%~dp0"

rem --- torch 2.7 на ZLUDA: без этого addmm с bias уходит в cublasLt -> NOT_SUPPORTED
set "DISABLE_ADDMM_CUDA_LT=1"

rem --- чтобы видеть компиляцию ядер, а не «тишину»
set "ZLUDA_COMGR_LOG_LEVEL=1"

rem --- кэш скомпилированных ядер. Смените путь при необходимости.
if not defined ZLUDA_CACHE_DIR set "ZLUDA_CACHE_DIR=%~dp0zluda-cache"

rem --- MIOpen: поменьше шума и поиск без зависаний (если cudnn выключен, не мешает)
set "MIOPEN_FIND_MODE=2"
set "MIOPEN_LOG_LEVEL=3"

rem --- не буферизовать вывод питона (иначе логи «залипают»)
set "PYTHONUNBUFFERED=1"

rem --- экспериментальные переключатели (раскомментировать по одному при отладке):
rem set "ZLUDA_WAVE64_SLOW_MODE=1"
rem set "TORCHDYNAMO_DISABLE=1"
rem set "TORCHINDUCTOR_DISABLE=1"
rem set "AMD_LOG_LEVEL=3"

rem --- аргументы ComfyUI под gfx803: минимум новых подсистем
set "COMMANDLINE_ARGS=--auto-launch --use-quad-cross-attention --reserve-vram 0.9 --disable-async-offload --disable-pinned-memory --disable-comfy-compiler --disable-dynamic-vram"

rem --- 8 ГБ VRAM: если ловите OOM на SDXL — добавьте --lowvram (или --novram)
rem set "COMMANDLINE_ARGS=%COMMANDLINE_ARGS% --lowvram"

rem --- точка входа: python из venv
set "PYTHON=%~dp0venv\Scripts\python.exe"
if not exist "%PYTHON%" (
    echo [ERROR] Не найден %PYTHON%
    echo         Запустите install-n.bat / install-for-older-amd.bat, либо поправьте PYTHON в этом файле.
    pause
    exit /b 1
)

echo ===========================================================================
echo  ComfyUI-Zluda / gfx803 / HIP 6.4
echo  torch lib:  %~dp0venv\Lib\site-packages\torch
echo  zluda dir:  %~dp0zluda
echo  cache dir:  %ZLUDA_CACHE_DIR%
echo  args:       %COMMANDLINE_ARGS%
echo ===========================================================================
echo.

if exist "%~dp0zluda\zluda.exe" (
    "%~dp0zluda\zluda.exe" -- "%PYTHON%" main.py %COMMANDLINE_ARGS%
) else (
    echo [WARN] zluda\zluda.exe не найден — запускаю python напрямую
    echo        (тогда nvcuda.dll должна лежать в venv\Scripts)
    "%PYTHON%" main.py %COMMANDLINE_ARGS%
)

set "RC=%ERRORLEVEL%"
echo.
echo ComfyUI завершился с кодом %RC%
pause
exit /b %RC%
