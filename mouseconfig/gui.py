"""
Простое графическое окно (tkinter) поверх HID++.

Запуск:  python -m mouseconfig gui
"""

from __future__ import annotations

import threading
import traceback
from datetime import datetime

from .hidpp import (
    FEATURE_REPROG_CONTROLS_V4,
    HidppDevice,
    HidppError,
    HidppTimeout,
    HidppTransport,
    enumerate_hidpp_devices,
)
from .cli import KNOWN_FEATURES, collect_diag, try_call


def run_gui(device_index: int = 0xFF, path=None) -> int:
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except ImportError:
        print("tkinter не установлен.\n"
              "Debian/Ubuntu: sudo apt install python3-tk\n"
              "Fedora:        sudo dnf install python3-tkinter\n"
              "Или пользуйтесь консолью: python -m mouseconfig diag")
        return 2

    root = tk.Tk()
    root.title("MouseConfig — настройка мыши")
    root.geometry("560x520")
    root.minsize(520, 480)

    state = {"dev": None, "transport": None}

    # ---------- верхняя панель: устройство ----------
    top = ttk.LabelFrame(root, text="Устройство")
    top.pack(fill="x", padx=10, pady=8)

    info_var = tk.StringVar(value="не подключено")
    ttk.Label(top, textvariable=info_var, justify="left").pack(
        anchor="w", padx=8, pady=(6, 2))

    ports = ttk.Frame(top)
    ports.pack(fill="x", padx=8, pady=(0, 8))
    port_combo = ttk.Combobox(ports, state="readonly", width=46)
    port_combo.pack(side="left", fill="x", expand=True)

    def refresh_ports():
        infos = enumerate_hidpp_devices()
        labels = [f"{i.product or 'Logitech'} · {i.path!r}" for i in infos]
        port_combo["values"] = labels
        port_combo.infos = infos
        if labels:
            port_combo.current(0)

    def connect(*_):
        idx = port_combo.current()
        infos = getattr(port_combo, "infos", [])
        if idx < 0 or idx >= len(infos):
            return
        disconnect()
        try:
            transport = HidppTransport(infos[idx].path)
            dev = HidppDevice(transport, device_index=device_index)
            name = try_call(dev.get_device_name, "") or infos[idx].product
            fw = try_call(dev.get_fw_version, "")
            battery = try_call(dev.get_battery)
            state["dev"], state["transport"] = dev, transport
            text = f"{name or 'Logitech'}"
            if fw:
                text += f"   прошивка {fw}"
            if battery:
                text += f"   заряд {battery['level']}%"
                if battery.get("charging"):
                    text += " (заряжается)"
            info_var.set(text)
            load_settings()
        except (HidppError, OSError) as exc:
            info_var.set("не подключено")
            messagebox.showerror("Ошибка подключения", str(exc))

    def disconnect():
        if state["transport"]:
            try:
                state["transport"].close()
            except Exception:
                pass
        state["dev"] = state["transport"] = None

    ttk.Button(ports, text="Обновить", command=refresh_ports).pack(
        side="left", padx=(6, 0))
    ttk.Button(ports, text="Подключить", command=connect).pack(
        side="left", padx=(6, 0))

    # ---------- DPI ----------
    dpi_frame = ttk.LabelFrame(root, text="Скорость (DPI)")
    dpi_frame.pack(fill="x", padx=10, pady=6)
    dpi_var = tk.IntVar(value=1000)
    dpi_label = ttk.Label(dpi_frame, text="1000")
    dpi_label.pack(side="right", padx=8)
    dpi_scale = ttk.Scale(dpi_frame, from_=200, to=8000, variable=dpi_var,
                          command=lambda _v: dpi_label.config(
                              text=str(int(float(dpi_var.get() or 0)))))
    dpi_scale.pack(side="left", fill="x", expand=True, padx=8, pady=8)

    def apply_dpi():
        dev = state["dev"]
        if not dev:
            return
        try:
            dev.set_dpi(int(float(dpi_var.get())))
            info_var.set(info_var.get())
        except HidppError as exc:
            messagebox.showwarning("DPI", f"Не удалось: {exc}")

    ttk.Button(dpi_frame, text="Применить",
               command=apply_dpi).pack(side="right", padx=4, pady=8)

    # ---------- колесо / SmartShift ----------
    wheel = ttk.LabelFrame(root, text="Колесо (SmartShift)")
    wheel.pack(fill="x", padx=10, pady=6)
    ss_var = tk.StringVar(value="auto")
    for text, value in (("Свободное", "free"), ("Ступенчатое", "ratchet"),
                        ("Авто", "auto")):
        ttk.Radiobutton(wheel, text=text, value=value, variable=ss_var,
                        command=lambda: apply_ss()).pack(side="left", padx=8,
                                                         pady=8)

    def apply_ss():
        dev = state["dev"]
        if not dev:
            return
        mode = {"free": 0, "ratchet": 1, "auto": 2}[ss_var.get()]
        try:
            dev.set_smart_shift(mode=mode)
        except HidppError as exc:
            messagebox.showwarning("Колесо", f"Не поддерживается: {exc}")

    # ---------- Easy-Switch ----------
    hosts = ttk.LabelFrame(root, text="Переключение между компьютерами "
                                      "(Easy-Switch)")
    hosts.pack(fill="x", padx=10, pady=6)
    for ch in range(1, 4):
        ttk.Button(hosts, text=f"Канал {ch}",
                   command=lambda ch=ch: switch_host(ch)).pack(
            side="left", padx=6, pady=8)

    def switch_host(ch):
        dev = state["dev"]
        if not dev:
            return
        try:
            dev.set_host(ch - 1)
        except HidppError as exc:
            messagebox.showwarning("Easy-Switch", f"Не поддерживается: {exc}")

    # ---------- служебное ----------
    bottom = ttk.Frame(root)
    bottom.pack(fill="x", padx=10, pady=10)

    def show_features():
        dev = state["dev"]
        if not dev:
            return
        rows = []
        for fid, label in KNOWN_FEATURES:
            index = try_call(lambda fid=fid: dev.get_feature_index(fid), 0)
            if index:
                rows.append(f"0x{fid:04X}  {label}  (index 0x{index:02X})")
        messagebox.showinfo("Возможности мыши",
                            "\n".join(rows) or "Ничего не найдено")

    def save_report():
        try:
            report = collect_diag(device_index, None)
        except (HidppError, OSError) as exc:
            messagebox.showerror("Отчёт", str(exc))
            return
        import json
        name = (report["device"].get("name") or "mouse").replace(" ", "_")
        default = f"mouse_report_{name}_{datetime.now():%Y%m%d_%H%M}.json"
        fn = filedialog.asksaveasfilename(
            defaultextension=".json", initialfile=default,
            filetypes=[("JSON", "*.json")])
        if not fn:
            return
        with open(fn, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2)
        messagebox.showinfo("Отчёт",
                            f"Сохранено:\n{fn}\n\nПришлите этот файл — "
                            "по нему будет собран профиль.")

    ttk.Button(bottom, text="Возможности",
               command=show_features).pack(side="left")
    ttk.Button(bottom, text="Сохранить отчёт для разработчика",
               command=save_report).pack(side="left", padx=8)

    status = tk.StringVar(value="Готово")
    ttk.Label(root, textvariable=status, anchor="w").pack(
        fill="x", padx=12, pady=(0, 8))

    def load_settings():
        dev = state["dev"]
        if not dev:
            return
        dpi = try_call(dev.get_dpi)
        if dpi:
            dpi_var.set(dpi)
            dpi_label.config(text=str(dpi))
        ss = try_call(dev.get_smart_shift)
        if ss:
            ss_var.set({0: "free", 1: "ratchet"}.get(ss["mode"], "auto"))

    refresh_ports()
    root.protocol("WM_DELETE_WINDOW", lambda: (disconnect(), root.destroy()))
    root.mainloop()
    return 0
