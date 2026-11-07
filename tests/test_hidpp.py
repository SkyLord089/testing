"""Тесты на разбор кадров HID++ без реального устройства (fake hid)."""

import sys
import types

# --- подменяем модуль hid до импорта пакета ---
fake_hid = types.ModuleType("hid")


class FakeHidDevice:
    def __init__(self, path=None):
        self.path = path
        self.written = []
        self.responses = []
        self.nonblocking = False

    def write(self, data):
        self.written.append(bytes(data))
        self.responses.append(self._build_response(bytes(data)))
        return len(data)

    def read(self, size, timeout_ms=0):
        if self.responses:
            return self.responses.pop(0)
        return b""

    def close(self):
        pass

    def _build_response(self, frame):
        report_id, device, feature, fnh = frame[0], frame[1], frame[2], frame[3]
        return bytes([report_id, device, feature, (fnh & 0xF0)]) + bytes(16)


fake_hid.Device = FakeHidDevice
fake_hid.enumerate = lambda: []
sys.modules.setdefault("hid", fake_hid)

from mouseconfig.hidpp import (  # noqa: E402
    HidppDevice,
    HidppTransport,
    data_to_str,
)
from mouseconfig.profiles import Profile, find_profile  # noqa: E402


def make_device():
    transport = HidppTransport(b"fake")
    return HidppDevice(transport)


def test_frame_format_long_report():
    dev = make_device()
    dev.t.read(0xFF, 0x05, 0x00, b"\x01", 0x3)
    frame = dev.t.hid.written[0]
    assert len(frame) == 20
    assert frame[0] == 0x11          # long report
    assert frame[1] == 0xFF          # receiver
    assert frame[2] == 0x05          # feature index
    assert frame[3] == 0x30          # sw_id 3 << 4 | function 0
    assert frame[4] == 0x01


def test_feature_index_is_cached():
    dev = make_device()
    dev.get_feature_index(0x1814)
    dev.get_feature_index(0x1814)
    # второй вызов не должен слать пакет в устройство
    assert len(dev.t.hid.written) == 1


def test_feature_not_supported():
    dev = make_device()
    # ответ с нулевым индексом = фичи нет
    dev.t.hid.responses = [bytes([0x11, 0xFF, 0x00, 0x10]) + bytes(16)]
    assert dev.has_feature(0x1B04) is False


def test_data_to_str():
    assert data_to_str(b"AB\x00C") == "AB.C"


def test_profile_matching():
    profile = Profile(model="MX", match_pid=[0xB034],
                      match_name_contains="MX Master")
    assert profile.matches("MX Master 3S", 0)
    assert profile.matches("Other", 0xB034)
    assert not profile.matches("Other", 0x1234)


def test_profile_dpi_range_defaults():
    profile = Profile()
    assert profile.dpi_range == (200, 8000, 50)
    profile.capabilities = {"dpi": {"min": 100, "max": 25600, "step": 50}}
    assert profile.dpi_range == (100, 25600, 50)


def test_find_profile_fallback():
    assert find_profile("Unknown Mouse", 0x9999).model.startswith("Универсальный")
