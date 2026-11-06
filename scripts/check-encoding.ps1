<#
.SYNOPSIS
    Проверяет кодировки всех скриптов в репозитории. Запускать ПЕРЕД работой.

.DESCRIPTION
    Ловит ровно ту ошибку, из-за которой PowerShell 5.1 падает с
    "Unexpected token" на русских строках:

      * .ps1 без BOM        -> PS 5.1 прочитает как ANSI и выдаст кракозябры
      * .ps1 не в UTF-8     -> то же самое
      * .bat с BOM          -> cmd.exe сломает первую строку
      * .bat не в CP866     -> кракозябры в консоли (не фатально, но грязно)
      * смешанные переводы строк -> cmd/PowerShell могут ругаться

    Ничего не меняет, только сообщает. Код возврата 1, если есть проблемы.

.PARAMETER Fix
    Исправить автоматически: добавить BOM, привести EOL к CRLF.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\check-encoding.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\check-encoding.ps1 -Fix
#>
[CmdletBinding()]
param(
    [switch]$Fix
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bomUtf8 = [byte[]](0xEF, 0xBB, 0xBF)

$problems = 0
$fixed = 0

function Report([string]$status, [string]$rel, [string]$detail) {
    $color = 'Green'
    if ($status -eq 'FAIL') { $color = 'Red' }
    elseif ($status -eq 'WARN') { $color = 'Yellow' }
    elseif ($status -eq 'FIX') { $color = 'Cyan' }
    Write-Host ("[{0,-4}] {1,-46} {2}" -f $status, $rel, $detail) -ForegroundColor $color
}

Write-Host ''
Write-Host ('-' * 78) -ForegroundColor DarkCyan
Write-Host '  ПРОВЕРКА КОДИРОВОК СКРИПТОВ' -ForegroundColor Cyan
Write-Host ('-' * 78) -ForegroundColor DarkCyan
Write-Host ("  Репозиторий: $repoRoot") -ForegroundColor DarkGray
Write-Host ''

# ---------- .ps1: UTF-8 с BOM + CRLF ----------
$ps1Files = Get-ChildItem -Path $repoRoot -Recurse -File -Filter '*.ps1' |
            Where-Object { $_.FullName -notmatch '\\\.git\\' }

Write-Host "-- .ps1: нужен UTF-8 С BOM + CRLF ($($ps1Files.Count) файлов)" -ForegroundColor White

foreach ($f in $ps1Files) {
    $rel = $f.FullName.Substring($repoRoot.Length + 1)
    $bytes = [IO.File]::ReadAllBytes($f.FullName)

    if ($bytes.Length -eq 0) {
        Report 'WARN' $rel 'пустой файл'
        continue
    }

    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $fileProblems = @()

    if (-not $hasBom) { $fileProblems += 'нет BOM' }

    # CRLF
    $lf = 0; $crlf = 0
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 0x0A) {
            $lf++
            if ($i -gt 0 -and $bytes[$i - 1] -eq 0x0D) { $crlf++ }
        }
    }
    if ($lf -ne $crlf) { $fileProblems += ("смешанные EOL ({0} LF, {1} CRLF)" -f $lf, $crlf) }

    if ($fileProblems.Count -eq 0) {
        Report 'OK' $rel ''
        continue
    }

    if (-not $Fix) {
        Report 'FAIL' $rel ($fileProblems -join '; ')
        $problems++
        continue
    }

    # ---- починка ----
    $body = $bytes
    if (-not $hasBom) { }
    # декодируем как UTF-8 (BOM отбрасываем), нормализуем EOL, пишем UTF-8 с BOM
    $text = [Text.Encoding]::UTF8.GetString($body)
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $text = $text.Replace("`r`n", "`n").Replace("`n", "`r`n")
    $enc = New-Object System.Text.UTF8Encoding($true)   # $true = с BOM
    [IO.File]::WriteAllText($f.FullName, $text, $enc)

    Report 'FIX' $rel ($fileProblems -join '; ')
    $fixed++
}

Write-Host ''

# ---------- .bat / .cmd: CP866 без BOM + CRLF ----------
$batFiles = Get-ChildItem -Path $repoRoot -Recurse -File -Include '*.bat', '*.cmd' |
            Where-Object { $_.FullName -notmatch '\\\.git\\' }

if ($batFiles.Count -gt 0) {
    Write-Host "-- .bat: нужен CP866 БЕЗ BOM + CRLF ($($batFiles.Count) файлов)" -ForegroundColor White
    foreach ($f in $batFiles) {
        $rel = $f.FullName.Substring($repoRoot.Length + 1)
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        $fileProblems = @()

        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            $fileProblems += 'BOM (cmd сломает первую строку)'
        }
        $lf = 0; $crlf = 0
        for ($i = 0; $i -lt $bytes.Length; $i++) {
            if ($bytes[$i] -eq 0x0A) {
                $lf++
                if ($i -gt 0 -and $bytes[$i - 1] -eq 0x0D) { $crlf++ }
            }
        }
        if ($lf -ne $crlf) { $fileProblems += ("смешанные EOL ({0} LF, {1} CRLF)" -f $lf, $crlf) }

        # UTF-8 русский текст в .bat выглядит как пары байт C0-FF -> проверяем эвристикой
        $utf8Pairs = 0
        for ($i = 0; $i -lt $bytes.Length - 1; $i++) {
            if ($bytes[$i] -ge 0xD0 -and $bytes[$i] -le 0xD1 -and $bytes[$i + 1] -ge 0x80 -and $bytes[$i + 1] -le 0xBF) {
                $utf8Pairs++
            }
        }
        if ($utf8Pairs -gt 3) { $fileProblems += "похоже на UTF-8 ($utf8Pairs пар) - нужен CP866" }

        if ($fileProblems.Count -eq 0) {
            Report 'OK' $rel ''
            continue
        }

        if (-not $Fix) {
            Report 'FAIL' $rel ($fileProblems -join '; ')
            $problems++
            continue
        }

        # починку .bat делает отдельный шаг: перекодировать в CP866 нужно из исходника
        Report 'WARN' $rel (($fileProblems -join '; ') + ' - правьте вручную (см. раздел 7 в docs\08-install-fresh.md)')
        $problems++
    }
    Write-Host ''
}

# ---------- итог ----------
Write-Host ('-' * 78) -ForegroundColor DarkCyan
if ($problems -eq 0) {
    Write-Host ("  ВСЁ В ПОРЯДКЕ. Исправлено: {0}" -f $fixed) -ForegroundColor Green
    Write-Host ''
    exit 0
}

Write-Host ("  ПРОБЛЕМ: {0}. Исправлено автоматически: {1}" -f $problems, $fixed) -ForegroundColor Red
Write-Host ''
Write-Host '  Запусти с -Fix, чтобы добавить BOM и починить переводы строк:' -ForegroundColor Yellow
Write-Host '    powershell -ExecutionPolicy Bypass -File .\scripts\check-encoding.ps1 -Fix' -ForegroundColor Yellow
Write-Host ''
exit 1
