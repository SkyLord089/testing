"""
Командная строка MouseConfig.

    python -m mouseconfig list                # найти беспроводные мыши Logitech
    python -m mouseconfig diag                # полная диагностика -> файл отчёта
    python -m mouseconfig battery             # заряд
    python -m mouseconfig dpi 1600            # установить DPI
    python -m mouseconfig smartshift auto     # режим колеса
    python -m mouseconfig host 2              # переключить канал Easy-Switch
    python -m mouseconfig captures            # показать нажатия боковых кнопок
    python -m mouseconfig features            # какие фичи поддерживает мышь
    python -m mouseconfig gui                 # графическое окно
"""

from __future__ import annotations

import argparse
import json
import platform
import sys
import time
from datetime import datetime

try:
    import hid
except ImportError:  # pragma: no cover
    hid = None

from .hidpp import (
    CHANGE_HOST_FN_GET_HOST_INFO,
    FEATURE_ADJUSTABLE_DPI,
    FEATURE_BATTERY_UNIFIED,
    FEATURE_BATTERY_VOLTAGE,
    FEATURE_CHANGE_HOST,
    FEATURE_DEVICE_FW_VERSION,
    FEATURE_DEVICE_NAME,
    FEATURE_HIRES_WHEEL,
    FEATURE_ONBOARD_PROFILES,
    FEATURE_REPROG_CONTROLS_V4,
    FEATURE_SMART_SHIFT,
    HidppDevice,
    HidppError,
    HidppTimeout,
    HidppTransport,
    enumerate_hidpp_devices,
)

# Известные фичи, которые имеет смысл перечислить в отчёте.
KNOWN_FEATURES = [
    (0x0005, "Device Name"),
    (0x0003, "Device FW Version"),
    (0x1000, "Battery Unified"),
    (0x1001, "Battery Voltage"),
    (0x1814, "Change Host (Easy-Switch)"),
    (0x1B04, "Reprogrammable Controls V4"),
    (0x8100, "Onboard Profiles"),
    (0x2200, "Mouse Pointer / speed"),
    (0x2201, "Adjustable DPI"),
    (0x2205, "Pointer Speed (enhanced)"),
    (0x2110, "Smart Shift"),
    (0x2121, "HiRes Wheel"),
    (0x1805, "ADC Measurement"),
    (0x1300, "LED Control"),
    (0x1E00, "Enable Hidden Features"),
    (0x1802, "Reset"),
    (0x1803, "Crown"),
    (0x1F20, "ADVANCED Gaming"),
    (0x1B05, "Persistent Remappable Action"),
    (0x1DF3, "Brightness Control"),
]


# --------------------------------------------------------------------------
# Подключение
# --------------------------------------------------------------------------
def open_device(device_index: int = 0xFF, path: str | None = None) -> HidppDevice:
    infos = enumerate_hidpp_devices()
    if not infos:
        raise SystemExit(
            "Не найдено ни одного HID++ интерфейса Logitech.\n"
            "Проверьте: приёмник Unifying/Bolt/Bluetooth вставлен, мышь включена.")
    if path:
        infos = [i for i in infos if str(i.path) == path] or infos
    last_error = None
    for info in infos:
        try:
            transport = HidppTransport(info.path)
            dev = HidppDevice(transport, device_index=device_index)
            name = dev.get_device_name()
            if name or device_index == 0xFF:
                return dev
        except (HidppError, OSError) as exc:
            last_error = exc
            continue
    raise SystemExit(f"Не удалось связаться с устройством: {last_error}")


def try_call(fn, default=None):
    try:
        return fn()
    except (HidppError, OSError):
        return default


# --------------------------------------------------------------------------
# Команды
# --------------------------------------------------------------------------
def cmd_list(args):
    infos = enumerate_hidpp_devices()
    if not infos:
        print("Устройств Logitech на HID++ не найдено.")
        return 1
    for i, info in enumerate(infos):
        print(f"[{i}] {info.product or '(без имени)'} "
              f"vid=0x{info.vendor_id:04X} pid=0x{info.product_id:04X}")
        print(f"    path={info.path!r}")
    return 0


def cmd_features(args):
    dev = open_device(args.device_index, args.path)
    print(f"Имя: {try_call(dev.get_device_name, '?')}")
    print(f"Прошивка: {try_call(dev.get_fw_version, '?')}")
    supported = []
    for fid, label in KNOWN_FEATURES:
        index = try_call(lambda fid=fid: dev.get_feature_index(fid), 0)
        mark = f"да (index 0x{index:02X})" if index else "нет"
        print(f"  0x{fid:04X}  {label:<32} {mark}")
        if index:
            supported.append({"feature_id": fid, "name": label,
                              "index": index})
    if args.json:
        print(json.dumps(supported, ensure_ascii=False, indent=2))
    return 0


def cmd_battery(args):
    dev = open_device(args.device_index, args.path)
    battery = try_call(dev.get_battery)
    if not battery:
        print("Заряд недоступен (мышь не сообщает).")
        return 1
    print(f"Заряд: {battery['level']}%"
          + (f" ({battery['voltage']} мВ, оценка)" if "voltage" in battery else "")
          + (" [заряжается]" if battery.get("charging") else ""))
    return 0


def cmd_dpi(args):
    dev = open_device(args.device_index, args.path)
    if args.value is None:
        print(f"Текущий DPI: {try_call(dev.get_dpi, 'не поддерживается')}")
        return 0
    dev.set_dpi(int(args.value))
    print(f"DPI установлен: {try_call(dev.get_dpi, '?')}")
    return 0


def cmd_smartshift(args):
    dev = open_device(args.device_index, args.path)
    if args.mode is None:
        print(f"SmartShift: {try_call(dev.get_smart_shift, 'не поддерживается')}")
        return 0
    mode = {"free": 0, "freespin": 0, "ratchet": 1, "freescroll": 0,
            "auto": 2}.get(args.mode.lower())
    if mode is None:
        raise SystemExit("Режим: free | ratchet | auto")
    dev.set_smart_shift(mode=mode)
    print(f"Готово: {try_call(dev.get_smart_shift, '?')}")
    return 0


def cmd_host(args):
    dev = open_device(args.device_index, args.path)
    if args.channel is None:
        print(f"Хосты: {try_call(dev.get_host_info, 'не поддерживается')}")
        return 0
    dev.set_host(int(args.channel) - 1)
    print(f"Переключено на канал {args.channel}: "
          f"{try_call(dev.get_host_info, '?')}")
    return 0


def cmd_captures(args):
    """Показать, какие события шлёт мышь (для отладки боковых кнопок)."""
    dev = open_device(args.device_index, args.path)
    print("Нажимайте кнопки/крутите колесо. Ctrl+C — выход.")
    print("Здесь видны только кнопки, включённые в HID++ reporting;"
          " большинство кликов идёт обычным HID-путём и сюда не попадает.")
    end = time.time() + args.seconds
    while time.time() < end:
        try:
            frame = dev.t.hid.read(64, timeout_ms=200)
        except Exception:
            frame = b""
        if frame:
            print("  raw:", bytes(frame).hex(" "))
    return 0


def collect_diag(device_index: int = 0xFF, path: str | None = None) -> dict:
    report = {
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "os": f"{platform.system()} {platform.release()} ({platform.machine()})",
        "python": platform.python_version(),
        "hidapi": getattr(hid, "__version__", "?"),
        "interfaces": [],
        "device": {},
    }
    for info in enumerate_hidpp_devices():
        report["interfaces"].append({
            "path": str(info.path),
            "product": info.product,
            "manufacturer": info.manufacturer,
            "vid": f"0x{info.vendor_id:04X}",
            "pid": f"0x{info.product_id:04X}",
        })

    dev = open_device(device_index, path)
    report["device"]["name"] = try_call(dev.get_device_name, "")
    report["device"]["firmware"] = try_call(dev.get_fw_version, "")
    report["device"]["battery"] = try_call(dev.get_battery)
    report["device"]["host_info"] = try_call(dev.get_host_info)
    report["device"]["dpi"] = try_call(dev.get_dpi)
    report["device"]["smart_shift"] = try_call(dev.get_smart_shift)
    report["device"]["features"] = []
    for fid, label in KNOWN_FEATURES:
        index = try_call(lambda fid=fid: dev.get_feature_index(fid), 0)
        if index:
            report["device"]["features"].append(
                {"id": f"0x{fid:04X}", "name": label, "index": index})
    # Сырой дамп ответа на пинг — помогает отличить HID++ 2.0 от 1.0
    try:
        report["device"]["ping"] = list(dev.t.read(dev.device_index, 0x00, 0x00,
                                                  b"\x00\x00", 0x1))
    except HidppError as exc:
        report["device"]["ping_error"] = str(exc)
    return report


def cmd_diag(args):
    report = collect_diag(args.device_index, args.path)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    name = (report["device"].get("name") or "mouse").replace(" ", "_")
    out = args.output or f"mouse_report_{name}.json"
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(text)
    print(f"\nОтчёт сохранён: {out}")
    print("Пришлите этот файл — по нему будет собран точный профиль.")
    return 0


def cmd_gui(args):
    from .gui import run_gui
    return run_gui(device_index=args.device_index, path=args.path)


# --------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="mouseconfig",
                                description="Настройка беспроводных мышей "
                                            "Logitech через HID++")
    p.add_argument("--device-index", type=lambda x: int(x, 0), default=0xFF,
                   help="индекс HID++ устройства (0xFF для приёмника)")
    p.add_argument("--path", default=None, help="HID-путь устройства")
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="список найденных интерфейсов").set_defaults(
        func=cmd_list)

    f = sub.add_parser("features", help="поддерживаемые фичи")
    f.add_argument("--json", action="store_true")
    f.set_defaults(func=cmd_features)

    sub.add_parser("battery", help="заряд").set_defaults(func=cmd_battery)

    d = sub.add_parser("dpi", help="DPI")
    d.add_argument("value", nargs="?", type=int)
    d.set_defaults(func=cmd_dpi)

    s = sub.add_parser("smartshift", help="режим колеса")
    s.add_argument("mode", nargs="?", help="free | ratchet | auto")
    s.set_defaults(func=cmd_smartshift)

    h = sub.add_parser("host", help="канал Easy-Switch")
    h.add_argument("channel", nargs="?", type=int)
    h.set_defaults(func=cmd_host)

    c = sub.add_parser("captures", help="поток событий кнопок")
    c.add_argument("--seconds", type=float, default=15)
    c.set_defaults(func=cmd_captures)

    dg = sub.add_parser("diag", help="отчёт о мыши в JSON")
    dg.add_argument("-o", "--output", default=None)
    dg.set_defaults(func=cmd_diag)

    sub.add_parser("gui", help="графическое окно").set_defaults(func=cmd_gui)
    return p


def main(argv: list[str] | None = None) -> int:
    if hid is None:
        print("Не установлен пакет hidapi:  pip install hidapi", file=sys.stderr)
        return 2
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except HidppTimeout as exc:
        print(f"Таймаут HID++: {exc}", file=sys.stderr)
        return 3
    except HidppError as exc:
        print(f"Ошибка HID++: {exc}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
