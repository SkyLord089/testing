# Сборка gmouse без «ручной» возни с линкером.
#
# Что делает:
#   1. проверяет, установлен ли Rust;
#   2. сначала пробует обычную сборку (MSVC);
#   3. если MSVC-линкера (link.exe) нет — сам ставит GNU-тулчейн
#      (ему не нужен Visual Studio) и собирает им;
#   4. печатает готовый путь к gmouse.exe.
#
# Запуск:
#   .\build.ps1
#   .\build.cmd            (то же самое, двойным щелчком)

param(
    [switch]$Clean,
    [switch]$ForceGnu
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Warn2($text){ Write-Host "    $text" -ForegroundColor Yellow }

# --- 1. Rust на месте? -----------------------------------------------------
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    Write-Host "Rust не найден." -ForegroundColor Red
    Write-Host @"
Установите Rust (это один раз, ~2 минуты):
  1) скачайте https://win.rustup.rs/x86_64
  2) запустите rustup-init.exe, нажмите Enter (вариант 1)
  3) закройте и снова откройте PowerShell, затем повторите: .\build.ps1

Либо просто скачайте готовый gmouse.exe:
  https://github.com/SkyLord089/testing/releases/latest
"@
    exit 1
}

Write-Step "Rust: $((cargo --version) -join ' ')"
if ($Clean -and (Test-Path target)) {
    Write-Step "Очистка target/"
    Remove-Item -Recurse -Force target
}

function Invoke-Cargo([string[]]$arguments) {
    & cargo @arguments
    return $LASTEXITCODE
}

# --- 2. Пробуем MSVC (по умолчанию на Windows) -----------------------------
$builtWith = $null
if (-not $ForceGnu) {
    Write-Step "Пробую собрать с MSVC-линкером"
    $log = & cargo build --release 2>&1
    $code = $LASTEXITCODE
    $log | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    if ($code -eq 0) {
        $builtWith = "msvc"
    } else {
        $text = ($log | Out-String)
        if ($text -match "link\.exe.*not found" -or $text -match "msvc targets depend") {
            Write-Warn2 "MSVC-линкер не найден — переключаюсь на GNU-тулчейн"
            Write-Warn2 "(Visual Studio для него не нужен)"
        } else {
            Write-Host "Сборка не удалась по другой причине (см. лог выше)." -ForegroundColor Red
            exit 1
        }
    }
}

# --- 3. GNU-тулчейн: ставим при необходимости ------------------------------
if (-not $builtWith) {
    $gnuToolchain = "stable-x86_64-pc-windows-gnu"
    $installed = (& rustup toolchain list) -join "`n"
    if ($installed -notmatch [regex]::Escape($gnuToolchain)) {
        Write-Step "Ставлю $gnuToolchain (около 300 МБ, один раз)"
        & rustup toolchain install $gnuToolchain --profile minimal
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Не удалось поставить GNU-тулчейн." -ForegroundColor Red
            Write-Host @"
Альтернатива — поставить компонент C++ для Visual Studio:
  winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
затем снова: .\build.ps1
"@
            exit 1
        }
    }
    Write-Step "Собираю GNU-тулчейном"
    & rustup run $gnuToolchain cargo build --release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $builtWith = "gnu"
}

# --- 4. Результат ---------------------------------------------------------
$exe = Join-Path $PSScriptRoot "target\release\gmouse.exe"
if (-not (Test-Path $exe)) {
    Write-Host "Сборка вроде прошла, но gmouse.exe не найден: $exe" -ForegroundColor Red
    exit 1
}
$size = [math]::Round((Get-Item $exe).Length / 1KB, 1)

Write-Host ""
Write-Ok "Готово ($builtWith): $exe ($size КБ)"
Write-Host ""
Write-Host "Запуск (обратите внимание на точку в начале имени):" -ForegroundColor Cyan
Write-Host "    .\target\release\gmouse.exe list"
Write-Host "    .\target\release\gmouse.exe test"
Write-Host "    .\target\release\gmouse.exe run"
Write-Host ""
Write-Host "Или просто используйте обёртку (она соберёт и запустит сама):" -ForegroundColor Cyan
Write-Host "    .\gmouse.cmd list"
Write-Host "    .\gmouse.cmd run"
