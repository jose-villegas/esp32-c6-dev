<#
.SYNOPSIS
    Sweeps SAND_BLOCK_W/SAND_BLOCK_H (launcher/main/apps/sand/sand.h),
    building+flashing+measuring each candidate on real hardware.

.DESCRIPTION
    For each (SAND_BLOCK_W, SAND_BLOCK_H) pair below: edits sand.h, runs
    the host test suite as a gate (skips the device run on failure -
    cheap, and catches most breakage before spending device time), builds
    and flashes build.diag, resets the device and captures its self-test
    output, then parses the settled-screen/flip/water frame-budget lines
    into a CSV. Restores sand.h and reflashes build.release in a `finally`
    block regardless of outcome, so a crash mid-sweep still leaves the
    repo and device in a known-good state.

    Background and the bugs this pattern hit before it worked cleanly:
    docs/Notes/Simulation-Lessons.md, "The sixth attempt" section.

.PARAMETER IdfExportPath
    Path to ESP-IDF's export.ps1. Defaults to this project's usual
    install location - override if yours differs.

.PARAMETER ComPort
    Serial port the device is on. Defaults to COM3.

.EXAMPLE
    .\block_size_sweep.ps1
    .\block_size_sweep.ps1 -ComPort COM7 -IdfExportPath C:\esp-idf\export.ps1
#>
param(
    [string]$IdfExportPath = "C:\Espressif\esp-idf-v5.5\export.ps1",
    [string]$ComPort = "COM3"
)

& $IdfExportPath | Out-Null

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher.
$launcher = Resolve-Path "$PSScriptRoot\..\..\..\.."
$sandH    = "$launcher\main\apps\sand\sand.h"
# The app's own results dir now that this script lives under the app -
# deleting main/apps/sand/ takes its scratch output with it too.
$results  = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force -Path $results | Out-Null
# capture_selftest.py is generic (just resets the device and captures serial
# output) and stayed in shared tools/sweeps/ when this script moved into the app.
$captureScript = "$launcher\tools\sweeps\capture_selftest.py"

function Write-Utf8NoBom($path, $content) {
    # Windows PowerShell 5.1's `Set-Content -Encoding utf8` writes a BOM,
    # silently polluting every future `git diff` of a file it touches.
    [System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding $false))
}

$original = Get-Content $sandH -Raw

$variants = @(
    @{ w = 8;  h = 32  },
    @{ w = 16; h = 32  },
    @{ w = 8;  h = 64  },
    @{ w = 16; h = 64  },
    @{ w = 32; h = 64  },
    @{ w = 32; h = 128 }
)

$resultsPath = "$results\block_size_sweep_results.csv"
"variant,settled_avg_us,flip_avg_us,flip_worst_us,water_avg_us,water_worst_us,selftest_failures" | Out-File -FilePath $resultsPath -Encoding utf8

Set-Location $launcher

try {
    foreach ($v in $variants) {
        $label = "$($v.w)x$($v.h)"
        Write-Host "=== Variant $label ==="

        $content = $original -replace '#define SAND_BLOCK_W \d+', "#define SAND_BLOCK_W $($v.w)"
        $content = $content -replace '#define SAND_BLOCK_H \d+', "#define SAND_BLOCK_H $($v.h)"
        Write-Utf8NoBom $sandH $content

        # Quick host-test gate before spending device time on a bad variant.
        # --login matters: bash.exe launched directly (not from an
        # interactive Git Bash session) does not source the profile that
        # puts coreutils (dirname, etc.) on PATH, and run_tests.sh needs it.
        $hostOut = (& "C:\Program Files\Git\usr\bin\bash.exe" --login -c "test/run_tests.sh" 2>&1 | Out-String)
        # NB: must be Out-String'd to a single string first - PowerShell's
        # -notmatch against a multi-line array return filters elements
        # rather than returning a boolean, which silently makes every
        # variant "fail" this gate regardless of the real result.
        if ($hostOut -notmatch "0 Failures") {
            Write-Host "HOST TESTS FAILED for $label - skipping device run"
            Write-Host $hostOut
            "$label,HOST_TEST_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        idf.py -B build.diag build *>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "BUILD FAILED for $label - skipping device run"
            "$label,BUILD_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        idf.py -B build.diag -p $ComPort flash *>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FLASH FAILED for $label - skipping capture"
            "$label,FLASH_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        $captureFile = "$results\block_size_capture_$label.txt"
        python $captureScript $captureFile --port $ComPort

        $text = Get-Content $captureFile -Raw

        # worst-step logging only exists on the sand-block-row-stagger
        # branch's tests, not main's - the (?:...)? group makes it
        # optional so either parses.
        $settled = [regex]::Match($text, "settled \d+x\d+ grid: (\d+) us per step")
        $flip    = [regex]::Match($text, "gravity flip on a \d+-grain pile, \d+x\d+: (\d+) us per step(?:, (\d+) us worst step)?")
        $water   = [regex]::Match($text, "water flowing on \d+x\d+: (\d+) us per step(?:, (\d+) us worst step)?")
        $fails   = [regex]::Match($text, "SELFTEST_COMPLETE failures=(\d+)")

        $settledAvg = if ($settled.Success) { $settled.Groups[1].Value } else { "MISSING" }
        $flipAvg    = if ($flip.Success)    { $flip.Groups[1].Value }    else { "MISSING" }
        $flipWorst  = if ($flip.Success -and $flip.Groups[2].Success)  { $flip.Groups[2].Value }  else { "N/A" }
        $waterAvg   = if ($water.Success)   { $water.Groups[1].Value }   else { "MISSING" }
        $waterWorst = if ($water.Success -and $water.Groups[2].Success) { $water.Groups[2].Value } else { "N/A" }
        $failCount  = if ($fails.Success)   { $fails.Groups[1].Value }   else { "MISSING" }

        "$label,$settledAvg,$flipAvg,$flipWorst,$waterAvg,$waterWorst,$failCount" | Add-Content -Path $resultsPath
        Write-Host "$label -> settled=$settledAvg flip=$flipAvg/$flipWorst water=$waterAvg/$waterWorst failures=$failCount"
    }
}
finally {
    # Always restore, regardless of outcome - crash, Ctrl+C, or normal completion.
    Write-Utf8NoBom $sandH $original
    idf.py -B build.release build *>$null
    idf.py -B build.release -p $ComPort flash *>$null
    Write-Host "=== sand.h restored, device reflashed with build.release from the original values ==="
}

Write-Host "=== DONE - results at $resultsPath ==="
Get-Content $resultsPath
