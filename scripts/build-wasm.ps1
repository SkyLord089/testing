# Run after changing src/lib.rs; the checked-in WASM makes normal runs offline.
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
cargo build --lib --release --target wasm32-unknown-unknown
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item target/wasm32-unknown-unknown/release/strafe_lab.wasm web/engine.wasm
Write-Host "Готово: web/engine.wasm. Пересоберите приложение через cargo run --release."
