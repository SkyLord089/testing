# Собирает сведения о мыши без установки Python (Windows, PowerShell).
# Запуск:  powershell -ExecutionPolicy Bypass -File collect_mouse_info.ps1
# Результат: mouse_report_windows.txt в текущей папке — пришлите его разработчику.

$out = Join-Path (Get-Location) "mouse_report_windows.txt"

"=== Mouse report ===" | Out-File $out -Encoding UTF8
"Date: $(Get-Date -Format s)" | Out-File $out -Append -Encoding UTF8
"OS:   $([System.Environment]::OSVersion.VersionString)" | Out-File $out -Append -Encoding UTF8
"" | Out-File $out -Append -Encoding UTF8

"--- PnP pointing devices ---" | Out-File $out -Append -Encoding UTF8
Get-PnpDevice -Class Mouse -PresentOnly |
    Select-Object Status, FriendlyName, InstanceId |
    Format-List | Out-String | Out-File $out -Append -Encoding UTF8

"--- HID interfaces (VID / PID / usage) ---" | Out-File $out -Append -Encoding UTF8
Get-PnpDevice -PresentOnly |
    Where-Object { $_.InstanceId -like "HID\VID_046D*" } |
    ForEach-Object {
        $id = $_.InstanceId
        $pid_ = if ($id -match "PID_([0-9A-F]{4})") { $matches[1] } else { "????" }
        $mi   = if ($id -match "MI_([0-9A-F]{2})") { $matches[1] } else { "--" }
        [pscustomobject]@{
            Status = $_.Status
            Name   = $_.FriendlyName
            VID    = "046D"
            PID    = "0x$pid_"
            MI     = $mi
        }
    } | Format-Table -AutoSize | Out-String -Width 200 |
    Out-File $out -Append -Encoding UTF8

"--- Logitech software installed ---" | Out-File $out -Append -Encoding UTF8
$paths = @(
  "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
  "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
  "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*"
)
Get-ItemProperty $paths -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -match "Logi" } |
    Select-Object DisplayName, DisplayVersion |
    Format-Table -AutoSize | Out-String | Out-File $out -Append -Encoding UTF8

Write-Host "Готово: $out"
Write-Host "Пришлите этот файл — по нему будет собран точный профиль мыши."
