# Quick serial monitor - skips idf.py's slow environment activation (~90 s).
#
#   powershell -File monitor.ps1                 # monitor the launcher
#   powershell -File monitor.ps1 -Port COM4      # a different port
#   powershell -File monitor.ps1 -Elf path.elf   # a different build
#
# Passing the .elf matters: it carries the debug symbols that turn a crash
# address into a source file and line. Ctrl+] quits.
param(
    [string]$Port = "COM3",
    [string]$Elf  = "$PSScriptRoot\launcher\build\launcher.elf"
)

$IdfPath      = "C:\Espressif\esp-idf-v5.5"
$venvPython   = "C:\Users\ville\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"
$toolchainBin = "C:\Users\ville\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"

if (-not (Test-Path $Elf)) {
    Write-Error "No ELF at $Elf - build it first with: idf.py build"
    exit 1
}

$env:PATH     = "$toolchainBin;$env:PATH"
$env:IDF_PATH = $IdfPath

& $venvPython "$IdfPath\tools\idf_monitor.py" `
    -p $Port -b 115200 `
    --toolchain-prefix riscv32-esp-elf- `
    --target esp32c6 `
    $Elf
