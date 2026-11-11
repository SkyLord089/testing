@echo off
rem ============================================================
rem  Общие переменные окружения набора.
rem
rem  Смысл файла - удержать ВСЕ кэши внутри каталога набора,
rem  чтобы установка не расползалась по диску. Подключается так:
rem      call "%~dp0env.bat"
rem  Намеренно БЕЗ setlocal: переменные должны остаться у вызывающего.
rem ============================================================

set "ZLRX_ROOT=%~dp0"
if "%ZLRX_ROOT:~-1%"=="\" set "ZLRX_ROOT=%ZLRX_ROOT:~0,-1%"

if not exist "%ZLRX_ROOT%\cache" mkdir "%ZLRX_ROOT%\cache" >nul 2>&1
if not exist "%ZLRX_ROOT%\tmp"   mkdir "%ZLRX_ROOT%\tmp"   >nul 2>&1

rem Временные файлы pip и сборки пакетов
set "TMP=%ZLRX_ROOT%\tmp"
set "TEMP=%ZLRX_ROOT%\tmp"

rem Кэш pip (иначе он уходит в %LocalAppData%\pip\Cache)
set "PIP_CACHE_DIR=%ZLRX_ROOT%\cache\pip"

rem Кэши PyTorch / HuggingFace / компиляторов
set "TORCH_HOME=%ZLRX_ROOT%\cache\torch"
set "TORCHINDUCTOR_CACHE_DIR=%ZLRX_ROOT%\cache\inductor"
set "TRITON_CACHE_DIR=%ZLRX_ROOT%\cache\triton"
set "HF_HOME=%ZLRX_ROOT%\cache\huggingface"
set "XDG_CACHE_HOME=%ZLRX_ROOT%\cache"

rem Кэш скомпилированных ядер ZLUDA.
rem Поддерживается только в ZLUDA 3.9.6 и новее: в 3.9.5 путь жёстко
rem зашит как dirs::cache_dir()/ZLUDA/ComputeCache, то есть %LocalAppData%.
set "ZLUDA_CACHE_DIR=%ZLRX_ROOT%\cache\zluda"
rem PyTorch никогда не вызывает cublasInit, а только через него ZLUDA доходит
rem до rocblas_initialize(). Без него rocBLAS остаётся в режиме ленивой
rem загрузки ядер, и на RX 580 умножение матриц возвращает нули.
set "ZLUDA_RX580_ROCBLAS_INIT=1"
rem rocBLAS ищет TensileLibrary рядом с rocblas.dll. Если HIP SDK лежит
rem нестандартно или библиотеку для gfx803 пришлось положить отдельно,
rem раскомментируйте и укажите свой каталог:
rem set "ROCBLAS_TENSILE_LIBPATH=%ZLRX_ROOT%\cache\rocblas"

set "HF_HUB_DISABLE_TELEMETRY=1"
set "DO_NOT_TRACK=1"

set "PYTHONPATH=%ZLRX_ROOT%;%PYTHONPATH%"
set "PYTHONIOENCODING=utf-8"
