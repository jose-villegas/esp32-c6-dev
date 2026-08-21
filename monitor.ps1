# Quick serial monitor for the ESP32-C6 hello_world project - skips idf.py's slow env activation.
param(
    [string]$Port = "COM3",
    [string]$Elf = "C:\Users\ville\Projects\esp32-c6\hello_world\build\hello_world.elf"
)

$venvPython = "C:\Users\ville\.espressif\python_env\idf5.4_py3.14_env\Scripts\python.exe"
$toolchainBin = "C:\Users\ville\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"
$env:PATH = "$toolchainBin;$env:PATH"
$env:IDF_PATH = "C:\Espressif\esp-idf"

& $venvPython "C:\Espressif\esp-idf\tools\idf_monitor.py" -p $Port -b 115200 --toolchain-prefix riscv32-esp-elf- --target esp32c6 $Elf
