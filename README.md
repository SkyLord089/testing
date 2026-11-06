# RX 580 + Windows 11: запуск ComfyUI через HIP SDK 6.4 и PyTorch 2.7+cu118

Этот репозиторий — набор скриптов и документации, чтобы на **AMD Radeon RX 580 (Polaris, gfx803)** под **Windows 11** запустить **ComfyUI** с **HIP SDK 6.4** и **PyTorch 2.7.0+cu118** через слой трансляции **ZLUDA**.

> [!IMPORTANT]
> Честно о совместимости — прочитайте до установки:
>
> - **HIP SDK 6.4 официально НЕ поддерживает gfx803 (RX 580)**. AMD не выпускает для него ядра rocBLAS в 6.4.
>   Работоспособность достигается **только патчем rocBLAS** от сообщества
>   ([advanced-lvl-up](https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU)),
>   собранным под HIP SDK 5.7.1 / 6.2.4 и работающим на 6.4 экспериментально.
> - **`torch==2.7.0+cu118` — это сборка PyTorch под CUDA (NVIDIA)**. На AMD GPU она работает **только через ZLUDA**
>   (трансляция CUDA → HIP в рантайме). Нативной ROCm-сборки PyTorch для Windows под gfx803 не существует.
> - Native-путь «официальный PyTorch + ROCm на Windows + RX 580» — не поддерживается ни AMD, ни PyTorch.
>   (Linux-only экспериментальные сборки gfx803: [spy1134/rocm-gfx803](https://github.com/spy1134/rocm-gfx803).)
>
> Короче: это **экспериментальная связка**, и она работает только при одновременном выполнении всех условий:
> HIP SDK (5.7.1 / 6.2.4 / 6.4-experimental) + патченный rocBLAS + ZLUDA + CUDA-сборка torch + правильные DLL в `torch\lib`.
> Официальной «магической кнопки» для gfx803 на Windows нет.

## Матрица проверенных конфигураций (сообщество, 2025–2026)

| Конфиг | Статус | Комментарий |
|---|---|---|
| HIP SDK **5.7.1** + ZLUDA 3.9.5 (rocm5) + патч rocBLAS + torch 2.2.1+cu118 | ✅ наиболее обкатанный путь | так работает [ComfyUI-Zluda](https://github.com/patientx/ComfyUI-Zluda) (`install-for-older-amd.bat`) |
| HIP SDK **6.2.4** + ZLUDA 3.9.5 (rocm6, nvcuda из rocm5) + патч rocBLAS + torch 2.7.0+cu118 | ✅ рабочая, с оговорками | патч rocBLAS собран именно под 6.2.4 |
| HIP SDK **6.4** + ZLUDA 3.9.5 (rocm6) + патч rocBLAS + torch 2.7.0+cu118 | ⚠️ экспериментально | ровно то, что вы хотели попробовать; патч собран под 6.2.4, применяется и на 6.4. **На torch 2.3+ обязателен `--disable-cuda-malloc`** (встроен в лаунчер, иначе fake OOM) — см. [docs/fake-oom.md](docs/fake-oom.md) |
| HIP SDK 7.x | ❌ | ZLUDA пока не поддерживает HIP SDK 7.x |
| torch **2.2.1**+cu118 (пин ComfyUI-Zluda) | ✅ самый скучный путь | если 2.7.0+cu118 не пойдёт даже с фиксами из docs/fake-oom.md — откатывайтесь сюда |
| torch **≥ 2.3** + ZLUDA **без** `--disable-cuda-malloc` | ❌ fake OOM | ComfyUI по умолчанию включает `backend:cudaMallocAsync` для torch≥2.0+cu*; ZLUDA его не тянет |
| torch +cu126 / +cu128 (вместо cu118) через ZLUDA | ⚠️ не проверялось широко | можно попробовать, поменяв `-TorchIndexUrl` |
| torch-directml | ✅ стабильный запасной вариант | работает на RX 580 через DirectX 12, без HIP и ZLUDA, заметно медленнее |

## Состав репозитория

```
scripts/
  Test-Prerequisites.ps1     — предстартовая проверка (OS, GPU, драйвер, Python, Git, HIP SDK, pagefile, диск)
  Install-ComfyUI-RX580.ps1  — основная установка: ComfyUI + venv + torch 2.7.0+cu118 + ZLUDA + патч DLL torch
  Patch-RocBLAS-gfx803.ps1   — (админ) бэкап и подмена rocBLAS на патч для gfx803 в HIP SDK
  Restore-RocBLAS.ps1        — (админ) полный откат патча rocBLAS
  Verify-Setup.ps1           — проверка: torch видит видеокарту через ZLUDA
docs/
  install_guide.md           — пошаговая установка (RU)
  fake-oom.md                — почему torch 2.3+ падает с «фальшивым» OOM и как это чинится (RU)
  troubleshooting.md         — типичные ошибки и их решения (RU)
  architecture.md            — как это работает, совместимость, что ожидать от железа (RU)
```

## Быстрый старт (Windows 11, PowerShell)

```powershell
git clone https://github.com/SkyLord089/testing.git
cd testing\scripts

# 1. Предстартовая проверка (ничего не меняет)
.\Test-Prerequisites.ps1

# 2. Патч rocBLAS под gfx803 — НУЖНЫ ПРАВА АДМИНИСТРАТОРА, HIP SDK 6.4 должен быть уже установлен
.\Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.4

# 3. Установка ComfyUI + torch 2.7.0+cu118 + ZLUDA и патч torch\lib
.\Install-ComfyUI-RX580.ps1

# 4. Проверка
.\Verify-Setup.ps1
```

Подробности, откаты и решение типичных проблем — в [docs/install_guide.md](docs/install_guide.md)
и [docs/troubleshooting.md](docs/troubleshooting.md).

## Честные ожидания по производительности

- RX 580 (8 ГБ, 256-бит, ~5.8/11.6 TFLOPS fp32/fp16-x4) — карта 2016–17 года.
- Первый запуск после установки или смены семплера/разрешений: **10–30 минут компиляции ядер ZLUDA/rocBLAS**, это норма.
- Дальше: SD 1.5 @ 512×512 — порядка 1–4 минут на изображение; SDXL тяжело; FLUX — только GGUF-кванты и невысокие разрешения.
- fp16 на gfx803 быстрый (в отличие от gfx900), поэтому `--force-fp16` обязателен.
- Включайте **большой pagefile (16+ ГБ)**: VAE-декод на 8 ГБ VRAM частенько уезжает в системную память и «нормально падает» без него.

## Источники и благодарности

- [lshqqytiger/ZLUDA](https://github.com/lshqqytiger/ZLUDA) — сама трансляция CUDA→HIP (v3.9.5, роуки сборки rocm5/rocm6)
- [advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU](https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU) — патченный rocBLAS/Tensile для gfx803/gfx900
- [patientx/ComfyUI-Zluda](https://github.com/patientx/ComfyUI-Zluda) (и HillQi fork) — обкатанный установщик старых AMD
- [sdnext wiki: ZLUDA](https://github.com/vladmandic/sdnext/wiki/ZLUDA) — процедура для HIP SDK 6.2/6.4
- [aktalascloud/polaris-gfx803-rx580-zluda-rocm-hip-comfyui-windows](https://github.com/aktalascloud/polaris-gfx803-rx580-zluda-rocm-hip-comfyui-windows) — готовый автоматический «тулкит» для ровно этого конфига
- [AMD HIP SDK](https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html) — официальный инсталлятор HIP SDK (скачивается вручную)

Никаких бинарников в репозитории не распространяется: все DLL скачиваются с первоисточников во время установки.
