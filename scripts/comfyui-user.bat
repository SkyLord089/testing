@echo off
rem ===========================================================================
rem  ComfyUI-Zluda launcher для RX 580 (gfx803) + HIP SDK 6.4 + torch 2.7+cu118
rem  Кладётся в корень ComfyUI-Zluda (рядом с main.py и venv\).
rem  Правьте только этот файл: штатные comfyui.bat/comfyui-n.bat перезапишутся
rem  при обновлении репозитория.
rem
rem  КОДИРОВКА: файл сохранён в CP866 без BOM. НЕ пересохраняйте его в UTF-8
rem  и не добавляйте BOM - cmd.exe прочитает такое как мусор.
rem ===========================================================================
setlocal EnableDelayedExpansion

cd /d "%~dp0"

rem --- torch 2.7 на ZLUDA: без этого addmm с bias уходит в cublasLt -> NOT_SUPPORTED
set "DISABLE_ADDMM_CUDA_LT=1"

rem --- чтобы видеть компиляцию ядер, а не тишину
set "ZLUDA_COMGR_LOG_LEVEL=1"

rem --- кэш скомпилированных ядер (смените путь при необходимости)
if not defined ZLUDA_CACHE_DIR set "ZLUDA_CACHE_DIR=%~dp0zluda-cache"

rem --- PATH k bin HIP SDK bolshe NE dobavlyaem.
rem     Prichina: comfy\zluda.py v pervyh zhe strokah delaet
rem       paths_no_rocm = [p for p in paths if "rocm" not in p.lower()]
rem     i vyrezaet iz PATH vse, chto soderzhit "rocm" - to est bin SDK.
rem     Poetomu comgr kladyotsya kopiey v venv\Scripts (etap 4 / patch),
rem     i imenno otuda ego beryot ZLUDA.


rem --- MIOpen: поменьше шума и поиск без зависаний
set "MIOPEN_FIND_MODE=2"
set "MIOPEN_LOG_LEVEL=3"

rem --- не буферизовать вывод питона
set "PYTHONUNBUFFERED=1"

rem --- экспериментальные переключатели (раскомментировать по одному при отладке):
rem set "ZLUDA_WAVE64_SLOW_MODE=1"
rem set "TORCHDYNAMO_DISABLE=1"
rem set "TORCHINDUCTOR_DISABLE=1"
rem set "AMD_LOG_LEVEL=3"

rem --- аргументы ComfyUI под gfx803: минимум новых подсистем
rem --- --disable-mmap: OBAZATELEN dlya ZLUDA.
rem     Bez nego safetensors ottaet tenzor, ssylayushiysya na mmap-fajl,
rem     a ZLUDA chitaet ego kak svoyu pamyat -> "Windows fatal exception:
rem     access violation" v torch\storage.py pri f.get_tensor(k).
rem     S flagom delaetsya prinuditelnaya kopiya (.to(device, copy=True)).
set "COMMANDLINE_ARGS=--auto-launch --use-quad-cross-attention --reserve-vram 0.9 --disable-async-offload --disable-pinned-memory --disable-comfy-compiler --disable-dynamic-vram --disable-mmap"

rem --- VRAM: если 4 ГБ - раскомментируйте строку ниже.
rem     Точный объём: Диспетчер задач - Производительность - GPU - Выделенная память.
rem set "COMMANDLINE_ARGS=%COMMANDLINE_ARGS% --lowvram"

rem --- если и с --lowvram не хватает (SDXL, видео): --novram
rem set "COMMANDLINE_ARGS=%COMMANDLINE_ARGS% --novram"

rem --- кэш моделей на диск установки, а не на C:
if not defined HF_HOME set "HF_HOME=%~dp0..\_cache\huggingface"

rem --- точка входа
set "PYTHON=%~dp0venv\Scripts\python.exe"
if not exist "%PYTHON%" (
    echo [ERROR] Ne najden %PYTHON%
    echo         Zapustite scripts\install\04-comfyui-zluda.ps1 ili popravte PYTHON v etom fajle.
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
    echo [WARN] zluda\zluda.exe ne najden - zapuskayu python napryamuyu
    echo        (togda nvcuda.dll dolzhna lezhat v venv\Scripts)
    "%PYTHON%" main.py %COMMANDLINE_ARGS%
)

set "RC=%ERRORLEVEL%"
echo.
echo ComfyUI zavershilsya s kodom %RC%
pause
exit /b %RC%
