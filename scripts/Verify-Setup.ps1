#requires -Version 5.1
<#
.SYNOPSIS
  Verifies the RX 580 ComfyUI install: torch under ZLUDA, rocBLAS patch marker, launcher.
.PARAMETER InstallRoot
  Default: %USERPROFILE%\RX580-ComfyUI
.PARAMETER SmokeTest
  Additionally runs a tiny CUDA matmul through ZLUDA.
  NOTE: on first ever run this triggers kernel compilation for GEMM and can take MINUTES.
.PARAMETER MemoryReport
  Diagnoses the "fake OOM" problem: prints mem_get_info (free/total as seen by the driver),
  torch allocator backend, allocated/reserved GiB and the allocator settings in effect.
.EXAMPLE
  .\Verify-Setup.ps1 -MemoryReport
#>
[CmdletBinding()]
param(
    [string]$InstallRoot = (Join-Path $env:USERPROFILE 'RX580-ComfyUI'),
    [switch]$SmokeTest,
    [switch]$MemoryReport
)

$ErrorActionPreference = 'Continue'
$script:Fails = 0
function Ok($m)  { Write-Host ("  [OK]   " + $m) -ForegroundColor Green }
function Warn2($m){ Write-Host ("  [WARN] " + $m) -ForegroundColor Yellow }
function Fail($m){ Write-Host ("  [FAIL] " + $m) -ForegroundColor Red; $script:Fails++ }
function Info($m){ Write-Host ("         " + $m) -ForegroundColor DarkGray }

Write-Host 'RX 580 ComfyUI setup verification' -ForegroundColor White

$comfyDir = Join-Path $InstallRoot 'ComfyUI'
$venvPy   = Join-Path $comfyDir 'venv\Scripts\python.exe'
$zludaDir = Join-Path $InstallRoot 'zluda'
$zludaExe = Join-Path $zludaDir 'zluda.exe'
$launcher = Join-Path $InstallRoot 'run-comfyui.bat'

Write-Host ""; Write-Host '== Files' -ForegroundColor Cyan
if (Test-Path -LiteralPath $venvPy)   { Ok "venv python: $venvPy" } else { Fail "venv python not found at $venvPy - run .\Install-ComfyUI-RX580.ps1" }
if (Test-Path -LiteralPath $zludaExe) { Ok "ZLUDA: $zludaExe" }
else {
    Fail "zluda.exe not found at $zludaExe - run .\Install-ComfyUI-RX580.ps1"
    $zDir = Join-Path $InstallRoot 'zluda'
    if (Test-Path -LiteralPath $zDir) {
        $files = @(Get-ChildItem -LiteralPath $zDir -Recurse -File -ErrorAction SilentlyContinue)
        if ($files.Count -gt 0) {
            Info ("zluda dir contains {0} file(s):" -f $files.Count)
            $files | Sort-Object FullName | Select-Object -First 20 | ForEach-Object {
                Info ("    {0,10} B  {1}" -f $_.Length, $_.FullName)
            }
        } else {
            Info 'zluda dir exists but is EMPTY - the ZLUDA step did not install anything. Re-run .\Install-ComfyUI-RX580.ps1 (torch download is cached by pip).'
        }
    }
}
if (Test-Path -LiteralPath (Join-Path $comfyDir 'main.py')) { Ok 'ComfyUI main.py present' } else { Fail "ComfyUI not found at $comfyDir" }
if (Test-Path -LiteralPath $launcher) { Ok "launcher: $launcher" }     else { Warn2 "run-comfyui.bat missing at $launcher" }

Write-Host ""; Write-Host '== rocBLAS gfx803 patch' -ForegroundColor Cyan
$markerFound = $false
$base = Join-Path $env:ProgramFiles 'AMD\ROCm'
if (Test-Path -LiteralPath $base) {
    foreach ($d in (Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue)) {
        $m = Join-Path $d.FullName 'bin\rocblas\gfx803.zluda-patched'
        if (Test-Path -LiteralPath $m) { Ok ("patch marker: {0}" -f $m); $markerFound = $true }
    }
}
if (-not $markerFound) { Warn2 'No gfx803.zluda-patched marker under Program Files\AMD\ROCm\*\bin\rocblas - did you run .\Patch-RocBLAS-gfx803.ps1 (elevated)?' }

Write-Host ""; Write-Host '== torch under ZLUDA' -ForegroundColor Cyan
if ((Test-Path -LiteralPath $venvPy) -and (Test-Path -LiteralPath $zludaExe)) {
    $check = @'
import torch
print("torch_version=" + torch.__version__)
print("cuda_build=" + str(torch.version.cuda))
print("cuda_available=" + str(torch.cuda.is_available()))
if torch.cuda.is_available():
    print("device_name=" + torch.cuda.get_device_name(0))
    print("device_capability=" + ".".join(str(x) for x in torch.cuda.get_device_capability(0)))
'@
    $checkFile = Join-Path $env:TEMP ('verify-torch-' + [IO.Path]::GetRandomFileName() + '.py')
    Set-Content -LiteralPath $checkFile -Value $check -Encoding ASCII
    try {
        $out = & $zludaExe -- $venvPy $checkFile 2>&1 | Out-String
        $out -split "`r?`n" | ForEach-Object { Info $_ }
        if ($out -match 'cuda_available=True') { Ok 'torch.cuda.is_available() == True (ZLUDA path works)' }
        else { Fail 'torch.cuda.is_available() != True - see docs/troubleshooting.md (rocBLAS patch / PATH / nvcuda.dll)' }
        if ($out -match 'torch_version=([0-9.]+[^\r\n]*)') { Info ("torch: {0}" -f $Matches[1]) }
    } catch {
        Fail ("torch check crashed: {0}" -f $_.Exception.Message)
    } finally {
        Remove-Item -LiteralPath $checkFile -Force -ErrorAction SilentlyContinue
    }
} else {
    Fail 'Skipping torch check - install is incomplete.'
}

if ($MemoryReport) {
    Write-Host ""; Write-Host '== Memory report (fake-OOM + WinError 126/127 diagnostics)' -ForegroundColor Cyan
    if ((Test-Path -LiteralPath $venvPy) -and (Test-Path -LiteralPath $zludaExe)) {
        $torchLib2 = Join-Path $comfyDir 'venv\Lib\site-packages\torch\lib'

        # --- Locate ROCm bin dirs and report PATH membership up front ---
        Write-Host ""; Write-Host '== ROCm bin / PATH' -ForegroundColor Cyan
        $rocmBins = @()
        foreach ($rv in @('6.4', '6.2', '5.7')) {
            $p = Join-Path $env:ProgramFiles ("AMD\ROCm\$rv\bin")
            if (Test-Path -LiteralPath $p) { $rocmBins += $p }
        }
        $pathEntries = @($env:Path -split ';' | ForEach-Object { $_.TrimEnd('\') })
        foreach ($p in $rocmBins) {
            if ($pathEntries -contains $p.TrimEnd('\')) { Ok "$p is on PATH" }
            else { Fail "$p NOT on PATH - bare-name DLL loads (what ComfyUI needs) will fail" }
            foreach ($name in @('amdhip64.dll', 'hipblas.dll', 'rocblas.dll', 'hipsparse.dll', 'rocsparse.dll', 'rocsolver.dll')) {
                $full = Join-Path $p $name
                if (Test-Path -LiteralPath $full) {
                    try {
                        $vi = (Get-Item -LiteralPath $full).VersionInfo.FileVersion
                        if (-not $vi) { $vi = '(no version resource)' }
                        Info ("{0,-20}  {1}" -f $vi, $full)
                    } catch { Info ("(no version info)  {0}" -f $full) }
                } else {
                    Warn2 "$name missing in $p"
                }
            }
        }
        Info 'note: amdhip64.dll absent from ROCm bin is normal - the AMD driver ships it in System32 (bare load OK confirms).'
        if ($rocmBins.Count -eq 0) { Warn2 'No ROCm bin directory found under Program Files (AMD\ROCm\{6.4,6.2,5.7}\bin)' }

        $report = @'
import os, ctypes, sys

zdir = os.environ.get("ZLUDA_DIAG_ZLUDA")
tdir = os.environ.get("ZLUDA_DIAG_TORCHLIB")
rbins = os.environ.get("ZLUDA_DIAG_ROCM") or ""

def tryload(p):
    try:
        ctypes.WinDLL(p)
        print("load OK: " + p)
    except OSError as e:
        print("load FAIL: %s -> %s" % (p, e))

def trybare(name):
    try:
        ctypes.WinDLL(name)
        print("bare load OK: " + name)
    except OSError as e:
        print("bare load FAIL: %s -> %s" % (name, e))

if zdir:
    for name in ("nvcuda.dll", "cublas.dll", "cusparse.dll", "nvrtc.dll"):
        p = os.path.join(zdir, name)
        if os.path.exists(p):
            tryload(p)
if tdir and os.path.isdir(tdir):
    for name in sorted(os.listdir(tdir)):
        low = name.lower()
        if low.startswith(("cublas64_11", "cublaslt64_11", "cusparse64_11", "nvrtc64_")):
            tryload(os.path.join(tdir, name))
for d in [x for x in rbins.split(";") if x]:
    for name in ("amdhip64.dll", "hipblas.dll", "rocblas.dll", "hipsparse.dll", "rocsparse.dll", "rocsolver.dll"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            tryload(p)
for name in ("amdhip64.dll", "hipblas.dll", "rocblas.dll", "hipsparse.dll", "rocsparse.dll", "rocsolver.dll"):
    trybare(name)

# ---------- PE import/export consistency check ----------
# Answers: which DLL (126) or which exact entry point (127) is missing,
# without any external tools. Only reads bytes, never loads anything.
import struct

def _pe(path):
    try:
        with open(path, 'rb') as fh:
            data = fh.read()
    except OSError:
        return None
    try:
        if data[:2] != b'MZ':
            return None
        e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
        if data[e_lfanew:e_lfanew + 4] != b'PE\0\0':
            return None
        nsec = struct.unpack_from('<H', data, e_lfanew + 6)[0]
        opt_size = struct.unpack_from('<H', data, e_lfanew + 20)[0]
        opt = e_lfanew + 24
        magic = struct.unpack_from('<H', data, opt)[0]
        plus = (magic == 0x20b)
        dd = opt + (112 if plus else 96)
        sec = opt + opt_size
        secs = []
        for i in range(nsec):
            s = sec + i * 40
            vs, va, rs, rp = struct.unpack_from('<IIII', data, s + 8)
            secs.append((va, vs, rs, rp))
        def rva2off(rva):
            for va, vs, rs, rp in secs:
                if va <= rva < va + max(vs, rs):
                    return rp + (rva - va)
            return None
        ddir = struct.unpack_from('<32I', data, dd)
        imp_rva, exp_rva = ddir[2], ddir[0]
        res = {'imports': [], 'exports': set()}
        if imp_rva:
            off = rva2off(imp_rva)
            for _ in range(512):
                oft, ts, fwd, name_rva, ft = struct.unpack_from('<IIIII', data, off)
                if oft == 0 and name_rva == 0 and ft == 0:
                    break
                no = rva2off(name_rva)
                dll = data[no:data.index(b'\0', no)].decode('ascii', 'replace') if no is not None else '?'
                funcs = []
                thunk = rva2off(oft) if oft else None
                if thunk:
                    step = 8 if plus else 4
                    fmt = '<Q' if plus else '<I'
                    mask = 0x8000000000000000 if plus else 0x80000000
                    for j in range(4096):
                        ent = struct.unpack_from(fmt, data, thunk + j * step)[0]
                        if ent == 0:
                            break
                        if ent & mask:
                            funcs.append('#%d' % (ent & 0xFFFF))
                            continue
                        hn = rva2off(ent)
                        if hn is None:
                            funcs.append('<bad rva>')
                            continue
                        funcs.append(data[hn + 2:data.index(b'\0', hn + 2)].decode('ascii', 'replace'))
                res['imports'].append((dll, funcs))
                off += 20
        if exp_rva:
            off = rva2off(exp_rva)
            f = struct.unpack_from('<IIHHIIIIIII', data, off)
            nfuncs, nnames, anam = f[6], f[7], f[9]
            noff = rva2off(anam)
            names = set()
            if noff is not None:
                for i in range(nnames):
                    nrva = struct.unpack_from('<I', data, noff + i * 4)[0]
                    no2 = rva2off(nrva)
                    if no2 is not None:
                        names.add(data[no2:data.index(b'\0', no2)].decode('ascii', 'replace'))
            res['exports'] = names
        return res
    except Exception:
        return None

def _search_dirs():
    dirs = []
    if zdir:
        dirs.append(zdir)
    if tdir:
        dirs.append(tdir)
    if rbins:
        dirs.extend([x for x in rbins.split(';') if x])
    windir = os.environ.get('WINDIR') or r'C:\Windows'
    dirs.append(os.path.join(windir, 'System32'))
    return dirs

def _find(dep):
    for d in _search_dirs():
        p = os.path.join(d, dep)
        if os.path.exists(p):
            return p
    return None

def depcheck(subject):
    info = _pe(subject)
    if not info:
        print("PE parse FAIL: " + subject)
        return
    print("deps of " + os.path.basename(subject))
    for dep, funcs in info['imports']:
        low = dep.lower()
        if low.startswith(('api-ms-', 'ext-ms-')):
            continue
        found = _find(dep)
        if not found:
            print("  DEP MISSING: %s (imported by %s)" % (dep, os.path.basename(subject)))
            continue
        dinfo = _pe(found)
        if not dinfo:
            print("  dep unreadable: %s -> %s" % (dep, found))
            continue
        missing = [f for f in funcs if not f.startswith('#') and f not in dinfo['exports']]
        if missing:
            print("  MISSING EXPORTS in %s (imported by %s): %s" % (found, os.path.basename(subject), ', '.join(missing[:12])))
        else:
            print("  dep OK: %s (%d imports)" % (found, len(funcs)))

subjects = []
if zdir:
    for name in ("nvcuda.dll", "cublas.dll", "cusparse.dll", "nvrtc.dll"):
        p = os.path.join(zdir, name)
        if os.path.exists(p):
            subjects.append(p)
if tdir and os.path.isdir(tdir):
    for name in sorted(os.listdir(tdir)):
        low = name.lower()
        if low.startswith(("cublas64_11", "cusparse64_11", "nvrtc64_")) and not low.endswith('.orig'):
            subjects.append(os.path.join(tdir, name))
if rbins:
    for d in rbins.split(';'):
        for name in ("amdhip64.dll", "hipblas.dll", "rocblas.dll", "hipsparse.dll", "rocsparse.dll", "rocsolver.dll"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                subjects.append(p)
for s in subjects:
    try:
        depcheck(s)
    except Exception as e:
        print("depcheck error for %s: %s" % (s, e))

try:
    import torch
except Exception as e:
    print("torch_import=FAIL %s: %s" % (type(e).__name__, e))
    sys.exit(0)

gib = 1024 ** 3
print("torch_import=OK torch=" + torch.__version__)
print("cuda_build=" + str(torch.version.cuda))
avail = torch.cuda.is_available()
print("cuda_available=" + str(avail))
if avail:
    free, total = torch.cuda.mem_get_info(0)
    print("mem_get_info: free=%.2f GiB total=%.2f GiB" % (free / gib, total / gib))
    print("allocated=%.3f GiB" % (torch.cuda.memory_allocated(0) / gib))
    print("reserved=%.3f GiB" % (torch.cuda.memory_reserved(0) / gib))
    print("max_allocated=%.3f GiB" % (torch.cuda.max_memory_allocated(0) / gib))
    print("allocator_backend=" + str(torch.cuda.get_allocator_backend()))
print("PYTORCH_CUDA_ALLOC_CONF=" + str(os.environ.get("PYTORCH_CUDA_ALLOC_CONF")))
print("PYTORCH_NO_CUDA_MEMORY_CACHING=" + str(os.environ.get("PYTORCH_NO_CUDA_MEMORY_CACHING")))
'@
        $reportFile = Join-Path $env:TEMP ('memreport-' + [IO.Path]::GetRandomFileName() + '.py')
        Set-Content -LiteralPath $reportFile -Value $report -Encoding ASCII
        try {
            $env:ZLUDA_DIAG_ZLUDA = $zludaDir
            $env:ZLUDA_DIAG_TORCHLIB = $torchLib2
            $env:ZLUDA_DIAG_ROCM = ($rocmBins -join ';')
            $out = & $zludaExe -- $venvPy $reportFile 2>&1 | Out-String
            $out -split "`r?`n" | ForEach-Object { Info $_ }
            if ($out -match 'torch_import=FAIL') {
                Fail 'torch import failed. See the load OK/FAIL lines and the deps of / MISSING EXPORTS report above: 126 = module missing (PATH / incomplete HIP SDK), 127 = entry point missing (ABI mismatch - on HIP 6.4 this is the known gfx803-patch/hipblas conflict, see the next line and docs/troubleshooting.md).'
                if (($out -match 'MISSING EXPORTS') -and ($out -match 'rocblas_[A-Za-z0-9]+_64')) {
                    Warn2 'Root cause: hipblas.dll 6.4 imports rocblas_*_64 entry points that the community gfx803 rocblas.dll (built for HIP 6.2.4/5.7.1) does NOT export - the confirmed WinError 127 trigger. Switch to HIP SDK 6.2.4: re-patch with .\Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.2, re-run installer with -RocmVersion 6.2, and keep only ROCm\6.2\bin in PATH. See docs/troubleshooting.md (WinError 127).'
                }
            } elseif ($out -match 'cuda_available=False') {
                Fail 'torch imports but CUDA is not available - see the load lines above; if all DLLs load OK, check rocBLAS patch marker and HIP SDK install.'
            } else {
                Ok 'torch imports fine and CUDA answers.'
            }
            if ($out -match 'mem_get_info: free=0\.00 GiB') {
                Warn2 'Driver reports 0 bytes free - classic fake-OOM signature. Run ComfyUI with --disable-cuda-malloc (see docs/fake-oom.md).'
            }
            if ($out -match 'allocator_backend=cudaMallocAsync') {
                Warn2 'allocator_backend=cudaMallocAsync - this is what ComfyUI forces for torch>=2.0+cu* and what ZLUDA cannot handle. Use --disable-cuda-malloc.'
            }
        } catch {
            Fail ("memory report crashed: {0}" -f $_.Exception.Message)
        } finally {
            $env:ZLUDA_DIAG_ZLUDA = $null
            $env:ZLUDA_DIAG_TORCHLIB = $null
            $env:ZLUDA_DIAG_ROCM = $null
            Remove-Item -LiteralPath $reportFile -Force -ErrorAction SilentlyContinue
        }

        Write-Host ""; Write-Host '== DLL versions' -ForegroundColor Cyan
        $watch = @((Join-Path $zludaDir 'zluda.exe'), (Join-Path $zludaDir 'nvcuda.dll'), (Join-Path $zludaDir 'cublas.dll'), (Join-Path $zludaDir 'cusparse.dll'), (Join-Path $zludaDir 'nvrtc.dll'))
        if (Test-Path -LiteralPath $torchLib2) {
            $watch += @(Get-ChildItem -LiteralPath $torchLib2 -Filter 'cublas64_11.dll' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
            $watch += @(Get-ChildItem -LiteralPath $torchLib2 -Filter 'cublasLt64_11.dll' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
            $watch += @(Get-ChildItem -LiteralPath $torchLib2 -Filter 'cusparse64_11.dll' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
            $watch += @(Get-ChildItem -LiteralPath $torchLib2 -Filter 'nvrtc64_*.dll' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
        }
        foreach ($f in $watch) {
            try {
                $fv = (Get-Item -LiteralPath $f).VersionInfo.FileVersion
                if ($fv) { Info ("{0,-20}  {1}" -f $fv, $f) } else { Info ("(no version resource)  {0}" -f $f) }
            } catch {
                Info ("(no version info)  {0}" -f $f)
            }
        }
    } else {
        Fail 'Skipping memory report - install is incomplete.'
    }
}

if ($SmokeTest) {
    Write-Host ""; Write-Host '== Smoke test (tiny matmul on GPU; may take minutes on first run)' -ForegroundColor Cyan
    $smoke = @'
import torch
a = torch.randn(512, 512, device="cuda")
b = a @ a
torch.cuda.synchronize()
print("matmul_ok shape=" + str(tuple(b.shape)) + " mean=" + str(float(b.mean())))
'@
    $smokeFile = Join-Path $env:TEMP ('smoke-torch-' + [IO.Path]::GetRandomFileName() + '.py')
    Set-Content -LiteralPath $smokeFile -Value $smoke -Encoding ASCII
    try {
        $out = & $zludaExe -- $venvPy $smokeFile 2>&1 | Out-String
        $out -split "`r?`n" | ForEach-Object { Info $_ }
        if ($out -match 'matmul_ok') { Ok 'CUDA matmul through ZLUDA succeeded' }
        else { Fail 'matmul did not report success - check docs/troubleshooting.md' }
    } finally {
        Remove-Item -LiteralPath $smokeFile -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
if ($script:Fails -gt 0) {
    Write-Host ("RESULT: {0} check(s) FAILED - see docs/troubleshooting.md" -f $script:Fails) -ForegroundColor Red
    exit 1
}
Write-Host 'RESULT: all checks passed. Launch with run-comfyui.bat - first run takes 10-30 minutes (kernel compilation).' -ForegroundColor Green
exit 0
