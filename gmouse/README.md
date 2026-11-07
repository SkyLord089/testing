# gmouse — настройка мыши под Windows 11

Утилита на Rust (без сторонних крейтов и без драйверов), которая настраивает
**любую** USB/Bluetooth-мышь: переназначает кнопки, меняет скорость курсора и
прокрутку, показывает HID-интерфейсы устройства и помогает найти
vendor-протокол аппаратного DPI.

Собрана под ваш отчёт: у вас **не Logitech**, а два HID-устройства

| VID:PID | Кто это |
|---------|---------|
| `1D57:2124` | Xenta / Eastern Times — 2.4 ГГц приёмник мыши |
| `1A2C:2511` | China Resource Semico / Eastern Times DS2511 |

Поэтому протокол Logitech HID++ (как в OpenMouse/Solaar) здесь неприменим —
`gmouse` работает с тем, что даёт сама Windows + HID-дескриптор устройства.

---

## Что реально работает «чётко» на ваших мышах

| Возможность | Статус |
|---|---|
| Переназначение всех кнопок (включая X1/X2, среднюю, наклоны колеса) | ✅ глобально, в любой программе |
| Двойной/тройной клик, прокрутка, горизонтальная прокрутка на кнопку | ✅ |
| Сочетания клавиш (`ctrl+shift+esc`), печать текста, запуск программ, ссылки | ✅ |
| Скорость курсора 1..20, строки прокрутки, ускорение указателя | ✅ |
| Показать точную модель, VID/PID, служебные интерфейсы | ✅ |
| Подобрать команду аппаратного DPI для неизвестного контроллера | ⚠️ через `probe`/`identify` — зависит от прошивки |
| Подсветка, макросы с задержками, профили в памяти мыши | ❌ только через драйвер производителя |

Важно: **аппаратный DPI** (800/1600/2400) у бюджетных мышей почти всегда
переключается кнопкой на корпусе или вообще не настраивается — это ограничение
прошивки, а не программы. `gmouse` даёт то, что Windows применяет к движению
(множитель скорости), и умеет искать vendor-команду DPI.

---

## Как получить gmouse.exe

### Вариант 1 — скачать готовый (проще всего)

**https://github.com/SkyLord089/testing/releases/latest**
→ файл `gmouse-windows.zip` (вход в GitHub не нужен, внутри уже собранный
`gmouse.exe` и запасной `gmouse-gnu.exe`).

Распакуйте, откройте PowerShell в этой папке:

```powershell
.\gmouse.exe list
```

### Вариант 2 — собрать у себя одной командой

Нужен только Rust (https://win.rustup.rs, установка «по Enter»):

```powershell
cd gmouse
.\build.cmd          # или:  powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Скрипт сам поймёт, чего не хватает:

* если установлен Visual Studio C++ — собирает обычным MSVC-тулчейном;
* **если MSVC-линкера нет (ваша ошибка `linker link.exe not found`) —
  доставит GNU-тулчейн и соберёт им, Visual Studio для этого не нужен.**

Ручной вариант на случай, если скрипт недоступен:

```powershell
rustup toolchain install stable-x86_64-pc-windows-gnu
rustup run stable-x86_64-pc-windows-gnu cargo build --release
```
(или поставьте компонент «Desktop development with C++» в Visual Studio
Build Tools и собирайте как обычно: `cargo build --release`).

### Вариант 3 — вообще не думать про сборку

```powershell
.\gmouse.cmd list      # соберёт при первом запуске и выполнит команду
.\gmouse.cmd run
```

### Почему была ошибка `linker link.exe not found`

Rust на Windows использует линкер от Visual Studio. Если установлен только
Rust без «Desktop development with C++», линкера нет. Решения:

| Способ | Что делать |
|---|---|
| GNU-тулчейн (быстро, ~300 МБ) | `rustup toolchain install stable-x86_64-pc-windows-gnu` и собрать с `--target x86_64-pc-windows-gnu` (скрипт `build.ps1` делает это сам) |
| Visual Studio Build Tools | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"` |
| Готовый exe | Releases, см. вариант 1 |

### Почему не запускался `target\release\gmouse.exe`

PowerShell не запускает программы по относительному пути без `\` перед
именем папки — он считает это именем модуля. Правильно:

```powershell
.\target\release\gmouse.exe list
```

Именно поэтому удобнее `gmouse.cmd` из папки проекта.

## Быстрый старт

```powershell
gmouse list                       # что за мышь подключена
gmouse status                     # текущие настройки и «эффективный CPI»
gmouse test                       # нажимайте кнопки — печатаются их имена
gmouse preset linux-back-forward  # X1/X2 = назад/вперёд как в Linux
gmouse run                        # применить (Ctrl+Alt+Q — стоп)
```

### Свои привязки

```powershell
gmouse bind x1 double_click
gmouse bind x2 hotkey "ctrl+shift+esc"
gmouse bind middle hotkey "alt+tab"
gmouse bind tilt_left hscroll -3
gmouse bind tilt_right hscroll 3
gmouse bind x1 app notepad.exe
gmouse bindings                    # что сохранено
gmouse unbind x1
```

Конфиг: `%APPDATA%\gmouse\config.json`

### Скорость и прокрутка

```powershell
gmouse speed 12        # 1..20, как ползунок в Параметрах Windows
gmouse scroll 5        # строк на деление колеса
gmouse accel off       # убрать ускорение указателя (для прицеливания)
gmouse hscroll 3       # символов горизонтальной прокрутки
```

### Аппаратный DPI (если хочется попробовать)

```powershell
# запустить от администратора
gmouse probe 1600      # отправить известные команды DPI, показать ответы
gmouse identify        # интерактивно: отвечаете «да/нет», программа
                       # находит рабочую команду и печатает описание
gmouse watch 15        # дамп входных репортов, сохранится в report_dump_*.txt
```

---

## Команды

```
gmouse list                     список мышей и интерфейсов
gmouse status                   настройки Windows для мыши
gmouse speed <1..20>            скорость курсора
gmouse scroll <строк>           строк на деление колеса
gmouse accel on|off             ускорение указателя
gmouse test                     определить имена кнопок
gmouse run                      демон с привязками
gmouse bind/unbind/bindings     управление привязками
gmouse preset <имя>             готовые наборы
gmouse dpi <значение>           аппаратный DPI (если протокол уже найден)
gmouse probe [dpi]              перебор команд DPI
gmouse identify                 подбор команды DPI (интерактивно)
gmouse watch <секунды>          дамп репортов
gmouse diag [-o файл]           отчёт JSON
gmouse profiles                 известные профили
gmouse config-path              путь к конфигу
```

**Кнопки:** `left`, `right`, `middle`, `x1`, `x2`, `wheel_up`, `wheel_down`,
`tilt_left`, `tilt_right`
**Действия:** `left/right/middle/x1/x2`, `double_click`, `triple_click`,
`wheel <±N>`, `hscroll <±N>`, `hotkey "ctrl+shift+esc"`, `text "..."`,
`app notepad.exe`, `url https://...`, `none`

Точные имена кнопок для вашей модели покажет `gmouse test`.

---

## Как устроено

```
gmouse/
├── Cargo.toml        без зависимостей
└── src/
    ├── main.rs       CLI и команды
    ├── win.rs        FFI к user32/kernel32/hid/setupapi (хуки, SendInput, SPI, HID)
    ├── hook.rs       WH_MOUSE_LL + WH_KEYBOARD_LL, переназначение
    ├── actions.rs    SendInput: клики, прокрутка, хоткеи, Unicode-текст, запуск
    ├── settings.rs   скорость курсора, прокрутка, ускорение (SystemParametersInfo)
    ├── hid.rs        SetupAPI + hid.dll: перечисление, репорты, feature/output
    ├── protocol.rs   поиск vendor-команды DPI (перебор + интерактив)
    ├── config.rs     конфиг/профили + собственный JSON парсер
    └── diag.rs       отчёт о мыши в JSON
```

Ключевые решения:

* **Хук, а не драйвер.** `WH_MOUSE_LL` работает глобально на уровне сессии:
  переназначение действует в браузере, играх и проводнике, ничего не
  устанавливая в систему.
* **Без зацикливания.** Синтезированные события несут метку `G_MOUSE_MARK` в
  `dwExtraInfo`, хук её видит и пропускает событие дальше. Нажатие «съедается»
  вместе с отпусканием — иначе залипали бы кнопки.
* **Без сетевых вызовов и телеметрии.** Только локальный Win32 API.
* **Нулевые зависимости.** Свой минимальный JSON-парсер (конфиг, профили,
  отчёты) — сборка не требует crates.io и работает офлайн.

### Ограничения, которые честно стоит знать

* Хук не перехватывает ввод в окнах, запущенных **от администратора**, если
  `gmouse` запущен без прав. Для игр с анти-читом запускайте `gmouse run`
  от администратора и учтите, что анти-читы могут блокировать хуки.
* Синтез ввода идёт через `SendInput` и не обходит UIPI/защищённые приложения.
* Аппаратный DPI меняется только если прошивка мыши это поддерживает.
* Подпись `.exe` отсутствует: Windows SmartScreen может предупредить при
  первом запуске («Подробнее → Выполнить в любом случае»).

---

## Проверка

```powershell
cd gmouse
cargo test          # 19 юнит-тестов: JSON, конфиг, привязки, DPI-команды
cargo clippy --all-targets
```

Тесты не требуют доступа к железу: проверяются парсинг, логика привязок,
сборка кадров DPI и профили.
