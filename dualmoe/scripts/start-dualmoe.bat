@echo off
chcp 65001 >nul
title DualMoE Studio (GPU Vulkan + CPU AVX2 + SSD Arena)
echo ============================================================
echo   Запуск DualMoE Studio (откроется http://127.0.0.1:8090)
echo   Для LearnHarness используйте URL: http://127.0.0.1:8090/v1
echo ============================================================
start "" "http://127.0.0.1:8090"
"%~dp0dualmoe-server.exe" --host 127.0.0.1 --port 8090 --ngl 99 --vram-cache-mb 2800 --cpu-share 0.92 --ram-cache-mb 6144 --ctx 24576 --threads 8 %*
pause
