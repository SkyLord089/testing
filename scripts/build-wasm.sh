#!/usr/bin/env sh
# Run after changing src/lib.rs; the checked-in WASM makes normal runs offline.
set -eu
cd "$(dirname "$0")/.."
cargo build --lib --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/strafe_lab.wasm web/engine.wasm
printf '%s\n' 'Готово: web/engine.wasm. Пересоберите приложение через cargo run --release.'
