$ErrorActionPreference = "Continue"

Write-Host "==> Compiling webauth_mm.dll using GCC..." -ForegroundColor Cyan

# CRITICAL: Force the MinGW bin folder into the current PATH so g++.exe doesn't silently crash
$mingwBin = "C:\msys64\mingw32\bin"
if ($env:PATH -notmatch [regex]::Escape($mingwBin)) {
    $env:PATH = "$mingwBin;" + $env:PATH
}

$includes = @(
    "-I./metamod-r/metamod/src",
    "-I./metamod-r/metamod/include/common",
    "-I./metamod-r/metamod/include/engine",
    "-I./metamod-r/metamod/include/dlls",
    "-I./metamod-r/metamod/include/pm_shared",
    "-I./rehlds/rehlds/public",
    "-I./rehlds/rehlds/public/rehlds"
)

$gccArgs = @(
    "-m32",
    "-shared",
    "-std=c++11",
    "-O2",
    "plugin.cpp"
) + $includes + @(
    "-lhiredis",
    "-lws2_32",
    "-Wl,--kill-at",
    "-static-libgcc",
    "-static-libstdc++",
    "-o",
    "webauth_mm.dll"
)

$compiler = "$mingwBin\g++.exe"

if (-Not (Test-Path $compiler)) {
    Write-Host "FATAL: 32-bit GCC not found at $compiler" -ForegroundColor Red
    exit 1
}

# Run the compiler natively, bypassing PowerShell's output swallowing
$process = Start-Process -FilePath $compiler -ArgumentList $gccArgs -NoNewWindow -Wait -PassThru

if ($process.ExitCode -eq 0) {
    Write-Host "`n==> Build successful: webauth_mm.dll generated." -ForegroundColor Green
} else {
    Write-Host "`n==> Build failed with Exit Code: $($process.ExitCode)." -ForegroundColor Red
    exit $process.ExitCode
}