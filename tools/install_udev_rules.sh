#!/usr/bin/env bash
# Разрешает доступ к Logitech HID++ без sudo (Linux / udev).
# Запуск:  sudo bash tools/install_udev_rules.sh
set -euo pipefail

RULE=/etc/udev/rules.d/99-logitech-hidpp.rules

cat > "$RULE" <<'EOF'
# Logitech HID++ (MouseConfig)
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="046d", MODE="0666", TAG+="uaccess"
# Приёмники Unifying / Bolt / Lightspeed (USB и Bluetooth)
SUBSYSTEM=="usb", ATTRS{idVendor}=="046d", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="bluetooth", ATTRS{idVendor}=="046d", MODE="0666", TAG+="uaccess"
EOF

udevadm control --reload-rules
udevadm trigger
echo "Установлено: $RULE"
echo "Отключите и снова подключите приёмник мыши."
