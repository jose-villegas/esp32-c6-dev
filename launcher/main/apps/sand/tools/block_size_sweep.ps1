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
    and the three landscape pour rows into a CSV. Restores sand.h and
    reflashes build.release in a `finally` block regardless of outcome, so
    a crash mid-sweep still leaves the repo and device in a known-good
    state.

    Background and the bugs this pattern hit before it worked cleanly:
    docs/sand/Simulation-Lessons.md, "The sixth attempt" section.

    WHY THE CANDIDATE LIST CHANGED. Every shape this script
    used to try was W <= 32 and H >= 32 - six tall blocks, no square, no
    transpose of any of them. 32x64 won that as the best of six, judged on
    scenes that all pour down grid +Y, while the board is played landscape,
    down grid +X. A search space with no wide block in it could not have
    found a landscape answer and would have kept naming the same winner
    however landscape the scenes became. The list below is closed under
    transpose plus the square, so an orientation bias cannot be baked into
    the shape of the search itself.

    WHAT THE WIDENED LIST ACTUALLY SAYS, host-ranked over the landscape
    rows: every transpose is WORSE (64x32 by 14%, 128x32 by 11%) and every
    narrower block is better (8x32 by 33%, 16x32 by 23%). The knob is W in
    both orientations - each block-level rejection in the hot loop spans
    along X in units of SAND_BLOCK_W, while H only decides how many grid
    rows share a block row. Landscape merely loses more of it, because more
    of the board is in motion. The list stays closed under transpose anyway:
    that is how the question got answered, not a prediction of the answer.

    WHAT CONSTRAINS A SHAPE: sand.c's _Static_assert covers SAND_BLOCK_W
    only - it must be a power of two, because dest_rows_full() recovers a
    block's span from x by masking. Nothing constrains SAND_BLOCK_H, which
    is only ever divided by. Separately, sand_priv.h's liquid invariant
    wants a block at least SAND_LIQUID_SIGHT wide, so W below 8 is not a
    candidate at all.

    RUN THE HOST PRE-SCREEN FIRST. Thirteen candidates here is a build,
    flash and capture apiece - hours of device time, with the board held
    throughout. block_size_prescreen.sh beside this file ranks the same
    candidates on the host in minutes, and this script is for confirming
    the two or three that survive it. The host ranks; only the device
    prices - see docs/sand/Perf-Instruments.md.

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
    @{ w = 8;   h = 32  },
    @{ w = 16;  h = 32  },
    @{ w = 8;   h = 64  },
    @{ w = 16;  h = 64  },
    @{ w = 32;  h = 64  },
    @{ w = 32;  h = 128 },
    # The transposes of all six above, and the square between them.
    @{ w = 32;  h = 8   },
    @{ w = 32;  h = 16  },
    @{ w = 64;  h = 8   },
    @{ w = 64;  h = 16  },
    @{ w = 64;  h = 32  },
    @{ w = 128; h = 32  },
    @{ w = 32;  h = 32  }
)

$resultsPath = "$results\block_size_sweep_results.csv"
"variant,settled_avg_us,flip_avg_us,flip_worst_us,water_avg_us,water_worst_us,land_water_avg_us,land_water_worst_us,land_deep_avg_us,land_deep_worst_us,land_sand_avg_us,land_sand_worst_us,selftest_failures" | Out-File -FilePath $resultsPath -Encoding utf8

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
        # The three landscape rows are the reason this sweep was re-run at
        # all: a candidate that only wins the portrait rows above answers
        # the question the old list was already answering.
        $landWater = [regex]::Match($text, "landscape water onto a sand bed, \d+x\d+: (\d+) us per step, worst single step (\d+) us")
        $landDeep  = [regex]::Match($text, "landscape water onto a deep sand bed, \d+x\d+: (\d+) us per step, worst single step (\d+) us")
        $landSand  = [regex]::Match($text, "landscape sand onto a sand bed, \d+x\d+: (\d+) us per step, worst single step (\d+) us")
        $fails   = [regex]::Match($text, "SELFTEST_COMPLETE failures=(\d+)")

        $settledAvg = if ($settled.Success) { $settled.Groups[1].Value } else { "MISSING" }
        $flipAvg    = if ($flip.Success)    { $flip.Groups[1].Value }    else { "MISSING" }
        $flipWorst  = if ($flip.Success -and $flip.Groups[2].Success)  { $flip.Groups[2].Value }  else { "N/A" }
        $waterAvg   = if ($water.Success)   { $water.Groups[1].Value }   else { "MISSING" }
        $waterWorst = if ($water.Success -and $water.Groups[2].Success) { $water.Groups[2].Value } else { "N/A" }
        $landWaterAvg   = if ($landWater.Success) { $landWater.Groups[1].Value } else { "MISSING" }
        $landWaterWorst = if ($landWater.Success) { $landWater.Groups[2].Value } else { "MISSING" }
        $landDeepAvg    = if ($landDeep.Success)  { $landDeep.Groups[1].Value }  else { "MISSING" }
        $landDeepWorst  = if ($landDeep.Success)  { $landDeep.Groups[2].Value }  else { "MISSING" }
        $landSandAvg    = if ($landSand.Success)  { $landSand.Groups[1].Value }  else { "MISSING" }
        $landSandWorst  = if ($landSand.Success)  { $landSand.Groups[2].Value }  else { "MISSING" }
        $failCount  = if ($fails.Success)   { $fails.Groups[1].Value }   else { "MISSING" }

        "$label,$settledAvg,$flipAvg,$flipWorst,$waterAvg,$waterWorst,$landWaterAvg,$landWaterWorst,$landDeepAvg,$landDeepWorst,$landSandAvg,$landSandWorst,$failCount" | Add-Content -Path $resultsPath
        Write-Host "$label -> settled=$settledAvg flip=$flipAvg/$flipWorst water=$waterAvg/$waterWorst land=$landWaterAvg/$landDeepAvg/$landSandAvg failures=$failCount"
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
