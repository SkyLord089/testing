# MouseConfig

Небольшая программа для настройки беспроводных мышей **Logitech** — аналог
OpenMouse / Solaar. Работает напрямую с мышью по протоколу **HID++ 2.0**
(приёмники Unifying / Bolt / LIGHTSPEED и Bluetooth), без Logitech Options.

Умеет:

* находить мышь и показывать её имя, прошивку, заряд батареи;
* менять **DPI** (скорость курсора);
* переключать режим колеса **SmartShift** (свободное / ступенчатое / авто);
* переключать **канал Easy-Switch** между компьютерами;
* показывать список поддерживаемых мышью функций;
* собирать **отчёт о мыши в JSON** — именно он нужен, чтобы сделать профиль
  под вашу конкретную модель;
* графическое окно (tkinter) с теми же возможностями.

---

## 1. Что мне прислать, чтобы программа работала «чётко» под вашу мышь

Коротко: **отчёт из этой же программы**. Он содержит всё, что нужно:
модель, PID/VID, версию протокола, список поддерживаемых функций и их
индексы, текущий DPI, режим колеса, состояние батареи.

### Вариант А — вы на Windows (Python не нужен)

1. Скачайте папку `tools/`.
2. Правой кнопкой по `collect_mouse_info.ps1` → «Выполнить с помощью
   PowerShell» (или в терминале:
   `powershell -ExecutionPolicy Bypass -File tools\collect_mouse_info.ps1`).
3. Пришлите появившийся файл `mouse_report_windows.txt`.

### Вариант Б — вы на Linux / macOS (нужен Python)

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install hidapi
# Linux: разрешить доступ к HID без sudo (один раз)
sudo bash tools/install_udev_rules.sh
# macOS: hidapi работает без дополнительных прав

python -m mouseconfig diag
```

Появится файл `mouse_report_<модель>.json` — пришлите его.

### Вариант В — просто расскажите словами

Если ставить ничего не хотите, достаточно ответить на 5 вопросов:

1. Точная модель (наклейка снизу: например `MX Master 3S`, `M720 Triathlon`,
   `G304`, `M185`, `M650`).
2. Как подключена: USB-приёмник (Unifying «оранжевая звезда» / Bolt /
   LIGHTSPEED) или Bluetooth.
3. Что именно нужно настраивать: DPI, колесо, боковые кнопки, подсветка,
   переключение между компьютерами, макросы?
4. ОС и версия (Windows 11, Ubuntu 24.04, macOS 15 …).
5. Что не устраивает сейчас (например: «колесо слишком тугое»,
   «боковые кнопки не работают в Linux», «хочу DPI 1200, а ставится 1000»).

> Важно: мыши **не** Logitech (Razer, SteelSeries, Defender, Bloody, A4Tech и
> т. п.) этим протоколом не управляются — у них свои, часто закрытые,
> протоколы. Напишите модель — скажу, что реально сделать.

---

## 2. Установка и запуск

```bash
pip install hidapi           # единственная зависимость
pip install -e .             # необязательно: появится команда mouseconfig
```

Запуск без установки:

```bash
python -m mouseconfig list          # какие HID++ интерфейсы найдены
python -m mouseconfig features      # что умеет мышь
python -m mouseconfig battery       # заряд
python -m mouseconfig dpi 1600      # поставить DPI
python -m mouseconfig dpi           # узнать текущий DPI
python -m mouseconfig smartshift auto      # free | ratchet | auto
python -m mouseconfig host 2        # переключить на канал 2
python -m mouseconfig diag -o my_mouse.json
python -m mouseconfig gui           # графическое окно
```

`--device-index 0xFF` (по умолчанию) — обращение через приёмник;
для Bluetooth-подключения иногда нужен `--device-index 1` или `2`.

Linux: клик по кнопке требует прав на hidraw — см.
`tools/install_udev_rules.sh`.

---

## 3. Как это устроено

```
mouseconfig/
├── hidpp.py       транспорт HID++ 2.0: кадры, таймауты, кэш фич, обёртки
│                  (battery, DPI, SmartShift, Easy-Switch, HiRes wheel)
├── cli.py         командная строка + сбор отчёта для разработчика
├── gui.py         графическое окно на tkinter
├── profiles.py    загрузка профилей моделей
└── profiles/      готовые профили: MX Master 3S / 2S, MX Anywhere 3,
                   G502 LIGHTSPEED, G304/G305, универсальный Bluetooth
tools/
├── collect_mouse_info.ps1   отчёт о мыши без Python (Windows)
└── install_udev_rules.sh    доступ к hidraw без sudo (Linux)
tests/
└── test_hidpp.py  тесты разбора кадров на поддельном hid-устройстве
```

Кадр HID++ 2.0 (длинный репорт, 20 байт):

```
0x11 | device index | feature index | (sw_id << 4) | function | 16 байт данных
```

Номер «слота» фичи выдаётся устройством по запросу `Root.getFeature`
(фича `0x0000`) и кэшируется — это и есть основной механизм протокола.

Полезные ID фич: `0x1000` батарея, `0x2201` DPI, `0x2110` SmartShift,
`0x1814` Easy-Switch, `0x1B04` переназначаемые кнопки, `0x8100` профили
в памяти мыши.

---

## 4. Ограничения (честно)

* Работает с **Logitech HID++ 2.0**. Старые устройства на HID++ 1.0
  (например, M185 с приёмником) увидят только базовые вещи.
* **Переназначение кнопок** (фича `0x1B04` / `0x8100`) — следующий шаг:
  в Windows это делается через перехват ввода, в Linux — через
  `evdev`/`uinput`. Сейчас программа только показывает, что фича есть.
* Подсветка (`0x1300`/`0x1DF3`) и игровые профили зависят от модели —
  включаются после того, как придёт отчёт.
* На macOS HID++-транспорт работает, но для глобального перехвата кнопок
  нужен Accessibility-доступ.

---

## 5. Тесты

```bash
pip install pytest
pytest -q
```

Тесты не требуют настоящей мыши: `hid` подменяется фейковым модулем,
проверяется формат кадров, кэширование фич и подбор профиля.
