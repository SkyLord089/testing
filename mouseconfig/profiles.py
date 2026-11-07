"""
Профиль устройства: какие настройки и как применять для конкретной модели.

Профиль — это обычный JSON-файл. Он описывает модель мыши и то,
какие возможности у неё есть, чтобы программа не «тыкала» наугад
в фичи, которых нет.

Схема профиля:
{
  "model": "MX Master 3S",
  "match": {"pid": ["0xB034", "0x4082"], "name_contains": "MX Master 3"},
  "device_index": 255,
  "capabilities": {
     "dpi":        {"min": 200, "max": 8000, "step": 50},
     "smart_shift": true,
     "hires_wheel": true,
     "easy_switch": {"hosts": 3},
     "battery": "unified"          // "unified" | "voltage" | false
  },
  "buttons": {                      // для будущего переназначения кнопок
     "middle": "middle_click",
     "back":   "browser_back",
     "forward":"browser_forward"
  }
}
"""

from __future__ import annotations

import glob
import json
import os
from dataclasses import dataclass, field

PROFILE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles")


@dataclass
class Profile:
    model: str = "Универсальный Logitech HID++"
    match_pid: list[int] = field(default_factory=list)
    match_name_contains: str = ""
    device_index: int = 0xFF
    capabilities: dict = field(default_factory=dict)
    buttons: dict = field(default_factory=dict)
    source: str = "(встроенный)"

    @property
    def dpi_range(self) -> tuple[int, int, int]:
        dpi = self.capabilities.get("dpi") or {}
        return (dpi.get("min", 200), dpi.get("max", 8000),
                dpi.get("step", 50))

    def matches(self, product: str, pid: int) -> bool:
        if self.match_pid and pid in self.match_pid:
            return True
        if self.match_name_contains and \
                self.match_name_contains.lower() in (product or "").lower():
            return True
        return False


def load_profiles(directory: str = PROFILE_DIR) -> list[Profile]:
    profiles = [Profile()]
    for path in sorted(glob.glob(os.path.join(directory, "*.json"))):
        try:
            with open(path, encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, json.JSONDecodeError):
            continue
        match = data.get("match", {})
        profiles.append(Profile(
            model=data.get("model", os.path.basename(path)),
            match_pid=[int(p, 0) for p in match.get("pid", [])],
            match_name_contains=match.get("name_contains", ""),
            device_index=int(data.get("device_index", 0xFF)),
            capabilities=data.get("capabilities", {}),
            buttons=data.get("buttons", {}),
            source=path,
        ))
    return profiles


def find_profile(product: str, pid: int) -> Profile:
    for profile in load_profiles():
        if profile.matches(product, pid):
            return profile
    return Profile()
