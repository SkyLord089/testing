# DualMoE v0.5.0 — нативное приложение вместо браузера

## Что изменилось

`DualMoE.exe` больше не открывает браузер. Запуск двойным кликом показывает **собственное окно приложения** (Edge WebView2 внутри Win32-окна): своя иконка в панели задач, заголовок «DualMoE v0.6.2», нет вкладок и адресной строки. Закрытие окна полностью останавливает движок.

## Архитектура (что где лежит)

| Файл | Роль |
| --- | --- |
| `dualmoe/src/app_http.h` / `app_http.cpp` | Ядро приложения: `AppOptions`, парсер командной строки, `AppServer` (движок + HTTP/OpenAI сервер + все маршруты). Раньше здесь был `main()`. |
| `dualmoe/src/main_desktop.cpp` | Единая точка входа: GUI-ветка (`wWinMain`, окно WebView2) и консольная ветка (браузер). |
| `dualmoe/src/win_dialogs.{h,cpp}` | Нативный системный диалог выбора `.gguf` (`IFileOpenDialog`), изолирован от HTTP-слоя. |
| `dualmoe/assets/dualmoe.rc`, `dualmoe.ico` | Иконка приложения + `VS_VERSION_INFO` (`0.6.2.0`). |
| `dualmoe/scripts/make_icon.py` | Пересборка `.ico` (16…256 px) из `assets/icon_source.png` (Pillow). |
| `dualmoe/cmake/x64-windows-msvc.cmake` | Toolchain для одиночного EXE: MSVC + статический CRT (`/MT`), AVX2/FMA/F16C. |

## Поведение по умолчанию

- Порт `8090`, но слушается **только `127.0.0.1`** в десктопной сборке (никакой сети наружу); консольная сборка по-прежнему на `0.0.0.0`.
- OpenAI-совместимый API `http://127.0.0.1:8090/v1` работает, пока окно открыто (LearnHarness / Hermes Agent).
- Модель грузится в фоне: окно появляется сразу и показывает 8 этапов загрузки, телеметрию и полётный журнал.
- Кнопка **«Файл модели…»** вызывает системный диалог Windows; встроенный «Диски…» остался для браузерного режима.
- Если WebView2 Runtime отсутствует: понятное сообщение + предложение `aka.ms/webview2` + автоматический откат к браузеру (без падения).
- `DualMoE.exe --browser` — тот же бинарник, но без окна (сервер + браузер).

## Сборочные цели

```bash
cmake -S dualmoe -B build-win -G "Ninja Multi-Config" \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/dualmoe/cmake/x64-windows-msvc.cmake \
      -DLLAMA_SOURCE_DIR=C:/llama-cpp -DGGML_VULKAN=ON
cmake --build build-win --config Release --target DualMoE        # окно, GUI-подсистема
cmake --build build-win --config Release --target dualmoe-server # консоль + браузер
```

`-DDUALMOE_DESKTOP_WINDOW=OFF` отключает окно (только консольный сервер).

## Проверки, которые прошли

- Локально (Linux): `dualmoe-tests` — все 11 гейтов (`ALL DUALMOE v0.6.2 GATES PASSED!`); `dualmoe-server` отдаёт UI 200, `/api/app/info` → `version 0.6.2`.
- Windows-ветки (`win_dialogs.cpp`, `main_desktop.cpp` с `DUALMOE_DESKTOP=1`) проверены синтаксически на моках Win32-заголовков.
- GitHub Actions `verify-gates` + `windows-exe`: MSVC x64 Vulkan сборка, PE-проверка «subsystem = 2 (GUI)», релиз `dualmoe-v0.6.2` с одним файлом `DualMoE.exe`.

## Известные ограничения

- WebView2 Runtime требуется (встроен в Windows 11; для старой Windows 10 — Evergreen Bootstrapper). При его отсутствии работает откат в браузер.
- Окно нельзя открепить во вторую вкладку/монитор как браузер: это одно окно приложения (так и задумано).
- Иконка в `.rc` использует `dualmoe.ico` относительно каталога `assets/` — при пересборке иконки запускайте `scripts/make_icon.py`.
