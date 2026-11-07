"""
Минимальная реализация протокола Logitech HID++ 2.0 (и частично 1.0).

Протокол работает по "длинным" HID-репортам (report ID 0x11, 20 байт)
или "коротким" (report ID 0x10, 7 байт) на интерфейсе USB, который
соответствует HID++ (обычно usage page 0xFF00, usage 0x0001).

Структура кадра HID++ 2.0 (report 0x11):
    [0] 0x11
    [1] device index (0xFF = приёмник Unifying/Bolt, 1..6 = устройство)
    [2] feature index (номер "слота" фичи, назначается через 0x0000)
    [3] function | (sw_id << 4)   (sw_id 0x1..0xF для HID++ 2.0)
    [4] параметры (16 байт)

Ошибки приходят как feature index 0xFF:
    [3] = код ошибки, [4] = feature index, [5] = function|sw_id.

Документация протокола: https://lekensteyn.nl/logitech-hidpp/
"""

from __future__ import annotations

import struct
import threading
import time
from dataclasses import dataclass

import hid

SHORT_REPORT_ID = 0x10
LONG_REPORT_ID = 0x11

DEVICE_INDEX_RECEIVER = 0xFF

# --- Feature / function IDs (из публичной документации HID++ 2.0) ---
FEATURE_ROOT = 0x0000
FEATURE_FEATURE_SET = 0x0001
FEATURE_DEVICE_NAME = 0x0005
FEATURE_RESET = 0x0020
FEATURE_DEVICE_FW_VERSION = 0x0003
FEATURE_BATTERY_UNIFIED = 0x1000
FEATURE_BATTERY_VOLTAGE = 0x1001
FEATURE_REPROG_CONTROLS_V4 = 0x1B04
FEATURE_CHANGE_HOST = 0x1814
FEATURE_ADC_MEASUREMENT = 0x1805
FEATURE_ONBOARD_PROFILES = 0x8100
FEATURE_MOUSE_POINTER = 0x2200
FEATURE_ADJUSTABLE_DPI = 0x2201
FEATURE_SMART_SHIFT = 0x2110
FEATURE_HIRES_WHEEL = 0x2121
FEATURE_HIRES_SCROLL = 0x2121  # то же самое, старое имя
FEATURE_LED_CONTROL = 0x1300

# Function IDs для reprogrammable controls V4
RPC_FN_GET_COUNT = 0
RPC_FN_GET_CID_INFO = 1
RPC_FN_GET_CID_REPORTING = 2
RPC_FN_SET_CID_REPORTING = 3
RPC_FN_GET_CID_GROUP = 4

# Function IDs для change host
CHANGE_HOST_FN_GET_HOST_INFO = 0
CHANGE_HOST_FN_SET_HOST = 1

# Function IDs для onboard profiles
ONBOARD_FN_GET_DESCRIPTION = 0
ONBOARD_FN_SET_MODE = 2
ONBOARD_FN_GET_MODE = 3
ONBOARD_FN_SET_SECTOR = 4
ONBOARD_FN_READ_SECTOR = 5
ONBOARD_FN_WRITE_SECTOR = 6
ONBOARD_FN_SECTOR_INFO = 7

# Function IDs для adjustable DPI
DPI_FN_GET_SENSOR_COUNT = 0
DPI_FN_GET_SENSOR_DPI_LIST = 1
DPI_FN_GET_SENSOR_DPI = 2
DPI_FN_SET_SENSOR_DPI = 3

# Function IDs для pointer speed / smart shift
POINTER_FN_GET = 0
POINTER_FN_SET = 1
SMART_SHIFT_FN_GET = 1
SMART_SHIFT_FN_SET = 2

# Function IDs для hires wheel
HIWHEEL_FN_GET_CAPABILITIES = 0
HIWHEEL_FN_GET_MODE = 1
HIWHEEL_FN_SET_MODE = 2


class HidppError(Exception):
    pass


class HidppTimeout(HidppError):
    pass


class HidppDeviceError(HidppError):
    """Устройство вернуло пакет-ошибку (feature 0xFF)."""


@dataclass
class HidppTransportInfo:
    path: bytes
    product_id: int
    vendor_id: int
    manufacturer: str
    product: str
    report_id: int = LONG_REPORT_ID


class HidppTransport:
    """Транспорт: один hidapi-интерфейс устройства + очередь ответов."""

    def __init__(self, path, report_id: int = LONG_REPORT_ID,
                 timeout: float = 1.5):
        self.path = path
        self.report_id = report_id
        self.timeout = timeout
        self._lock = threading.RLock()
        self._pending: dict[tuple[int, int], list[bytes]] = {}
        self._pending_lock = threading.Lock()
        self._stop = threading.Event()

        self.hid = hid.Device(path=path)
        self.hid.nonblocking = True
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # -- служебное ---------------------------------------------------
    def _read_loop(self):
        while not self._stop.is_set():
            try:
                data = self.hid.read(64, timeout_ms=100)
            except Exception:
                time.sleep(0.05)
                continue
            if not data:
                continue
            self._dispatch(bytes(data))

    def _dispatch(self, raw: bytes):
        if len(raw) < 4 or raw[0] not in (SHORT_REPORT_ID, LONG_REPORT_ID):
            return
        device = raw[1]
        feature = raw[2]
        sw_id = raw[3] & 0x0F
        payload = raw[4:]
        # Ошибки (feature == 0xFF) кладём в отдельную очередь, чтобы их
        # мог подхватить любой ожидающий вызов.
        key = (device, 0xFFFF) if feature == 0xFF else (device, feature)
        with self._pending_lock:
            self._pending.setdefault(key, []).append((sw_id, payload))
            # не даём очереди расти, если ответ никто не забрал
            if len(self._pending[key]) > 64:
                del self._pending[key][:-16]

    def close(self):
        self._stop.set()
        try:
            self.hid.close()
        except Exception:
            pass

    def read(self, device: int, feature: int, function: int,
             params: bytes, sw_id: int) -> bytes:
        """Отправить запрос и дождаться ответа. Возвращает 16 байт параметров."""
        length = 16 if self.report_id == LONG_REPORT_ID else 3
        params = (params + b"\x00" * length)[:length]
        frame = bytes([self.report_id, device, feature,
                       ((sw_id & 0x0F) << 4) | (function & 0x0F)]) + params
        frame = frame + b"\x00" * (20 - len(frame)) if self.report_id == LONG_REPORT_ID else frame

        with self._lock:
            with self._pending_lock:
                self._pending.pop((device, feature), None)
                self._pending.pop((device, 0xFFFF), None)
            self.hid.write(frame)

            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline:
                with self._pending_lock:
                    queue = self._pending.get((device, feature), [])
                    received = queue.pop(0) if queue else None
                    errs = self._pending.get((device, 0xFFFF), [])
                    err = errs.pop(0) if errs else None
                if received is not None:
                    return received[1]
                if err is not None:
                    code = err[1][0]
                    raise HidppDeviceError(
                        f"HID++ error 0x{code:02X} "
                        f"(feature 0x{err[1][1]:02X}, fn 0x{err[1][2]:02X})")
                time.sleep(0.001)
        raise HidppTimeout(
            f"нет ответа: устройство {device}, feature 0x{feature:04X}, "
            f"function {function}")


def enumerate_hidpp_devices() -> list[HidppTransportInfo]:
    """Найти все интерфейсы, похожие на HID++ (Logitech)."""
    found = []
    for info in hid.enumerate():
        if info.get("vendor_id") != 0x046D:
            continue
        usage_page = info.get("usage_page") or 0
        usage = info.get("usage") or 0
        # HID++: vendor-defined страница 0xFF00, usage 0x0001 / 0x0002.
        # У части устройств hidapi не отдаёт usage_page — тогда пробуем
        # по номеру интерфейса: HID++ обычно на interface_number == 1 или 2.
        looks_like = (usage_page == 0xFF00 and usage in (0x0001, 0x0002, 0x0003))
        if not looks_like:
            iface = info.get("interface_number", -1)
            if iface in (1, 2) and (info.get("usage_page") or 0) >= 0xFF00:
                looks_like = True
        found.append(HidppTransportInfo(
            path=info["path"],
            product_id=info.get("product_id", 0),
            vendor_id=info.get("vendor_id", 0),
            manufacturer=info.get("manufacturer_string") or "",
            product=info.get("product_string") or "",
            report_id=LONG_REPORT_ID,
        ))
    # убираем дубли путей
    uniq, seen = [], set()
    for f in found:
        if f.path not in seen:
            seen.add(f.path)
            uniq.append(f)
    return uniq


class HidppDevice:
    """Высокоуровневый объект устройства: кэш фич, типовые вызовы."""

    def __init__(self, transport: HidppTransport, device_index: int = 0xFF):
        self.t = transport
        self.device_index = device_index
        self._features: dict[int, int] = {}
        self._sw_id = 0x1
        self._sw_lock = threading.Lock()

    # -- низкий уровень ----------------------------------------------
    def _next_sw_id(self) -> int:
        with self._sw_lock:
            self._sw_id = 0x1 if self._sw_id >= 0x0F else self._sw_id + 1
            return self._sw_id

    def call(self, feature_id: int, function: int, params: bytes = b"") -> bytes:
        index = self.get_feature_index(feature_id)
        if index == 0:
            raise HidppError(f"фича 0x{feature_id:04X} не поддерживается")
        return self.t.read(self.device_index, index, function, params,
                           self._next_sw_id())

    def call_raw(self, feature_index: int, function: int,
                 params: bytes = b"") -> bytes:
        return self.t.read(self.device_index, feature_index, function, params,
                           self._next_sw_id())

    # -- корень протокола --------------------------------------------
    def ping(self) -> tuple[str, int]:
        # Root.getFeature(0x0000) -> feature index 0 и версия протокола
        res = self.t.read(self.device_index, 0x00, 0x00, b"\x00\x00",
                          self._next_sw_id())
        proto_major = res[0] >> 4
        proto_minor = (res[0] & 0x0F) << 4 | (res[1] >> 4)
        return f"{proto_major}.{proto_minor}", res[2]

    def get_feature_index(self, feature_id: int) -> int:
        if feature_id in self._features:
            return self._features[feature_id]
        # Root.getFeature: params = feature_id (big endian)
        res = self.t.read(self.device_index, 0x00, 0x00,
                          struct.pack(">H", feature_id), self._next_sw_id())
        index = res[0]
        self._features[feature_id] = index
        return index

    def has_feature(self, feature_id: int) -> bool:
        try:
            return self.get_feature_index(feature_id) != 0
        except HidppError:
            return False

    # -- удобные обёртки ---------------------------------------------
    def get_device_name(self) -> str:
        try:
            ln = self.call(FEATURE_DEVICE_NAME, 0)[0]
            data = self.call(FEATURE_DEVICE_NAME, 1)
            return data[:ln].decode("utf-8", "replace")
        except HidppError:
            return ""

    def get_fw_version(self) -> str:
        try:
            res = self.call(FEATURE_DEVICE_FW_VERSION, 0)
            # entity count, entity index... упрощённо: активная прошивка
            res2 = self.call(FEATURE_DEVICE_FW_VERSION, 0, bytes([1]))
            name_len = res2[1]
            version = res2[2:4][::-1]
            return (f"{version[0]}.{version[1]:02d}"
                    f"{('.' + data_to_str(res2[4:4 + min(name_len, 8)])) if name_len else ''}")
        except HidppError:
            return ""

    def get_battery(self):
        """Возвращает dict(level=..., charging=..., voltage=..., status=...)."""
        out = {}
        try:
            res = self.call(FEATURE_BATTERY_UNIFIED, 0)
            level = res[0]
            out["level"] = level
            out["charging"] = bool(res[2] & 0x01) if len(res) > 2 else None
            return out
        except HidppError:
            pass
        try:
            res = self.call(FEATURE_BATTERY_VOLTAGE, 0)
            voltage = struct.unpack(">H", res[0:2])[0]
            flags = res[2]
            level = 0
            if voltage >= 4300:
                level = 100
            elif voltage <= 3600:
                level = 0
            else:
                level = int((voltage - 3600) * 100 / 700)
            return {"level": level, "charging": bool(flags & 0x80),
                    "voltage": voltage, "estimated": True}
        except HidppError:
            return None

    def get_host_info(self):
        try:
            res = self.call(FEATURE_CHANGE_HOST, CHANGE_HOST_FN_GET_HOST_INFO)
            return {"num_hosts": res[0], "current_host": res[1],
                    "flags": res[2]}
        except HidppError:
            return None

    def set_host(self, host: int) -> bool:
        self.call(FEATURE_CHANGE_HOST, CHANGE_HOST_FN_SET_HOST, bytes([host]))
        return True

    def get_dpi(self) -> int | None:
        try:
            res = self.call(FEATURE_ADJUSTABLE_DPI, DPI_FN_GET_SENSOR_DPI,
                            b"\x00")
            return struct.unpack(">H", res[1:3])[0]
        except HidppError:
            return None

    def set_dpi(self, dpi: int) -> bool:
        self.call(FEATURE_ADJUSTABLE_DPI, DPI_FN_SET_SENSOR_DPI,
                  struct.pack(">BH", 0, dpi))
        return True

    def get_smart_shift(self):
        try:
            res = self.call(FEATURE_SMART_SHIFT, SMART_SHIFT_FN_GET)
            return {"mode": res[0], "auto_disengage": res[1] * 500,
                    "default": res[2] * 500}
        except HidppError:
            return None

    def set_smart_shift(self, mode: int | None = None,
                       auto_disengage_ms: int | None = None,
                       default_ms: int | None = None) -> bool:
        cur = self.get_smart_shift()
        if cur is None:
            raise HidppError("Smart Shift не поддерживается")
        params = bytes([
            cur["mode"] if mode is None else mode,
            int((cur["auto_disengage"] if auto_disengage_ms is None
                 else auto_disengage_ms) / 500),
            int((cur["default"] if default_ms is None else default_ms) / 500),
        ])
        self.call(FEATURE_SMART_SHIFT, SMART_SHIFT_FN_SET, params)
        return True

    def get_hires_wheel_mode(self) -> int | None:
        try:
            return self.call(FEATURE_HIRES_WHEEL, HIWHEEL_FN_GET_MODE)[0]
        except HidppError:
            return None

    def set_hires_wheel_mode(self, mode: int) -> bool:
        self.call(FEATURE_HIRES_WHEEL, HIWHEEL_FN_SET_MODE, bytes([mode]))
        return True


def data_to_str(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data)
