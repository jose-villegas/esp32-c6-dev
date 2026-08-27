<#
.SYNOPSIS
    Sweeps GATHER_MAX_PIXELS (main/gfx_dirty.h).

.DESCRIPTION
    Bounds the biggest leaf-refined split gather_and_send() will attempt
    before falling back to a coarser send. Same build+flash+measure+
    restore pattern as main/apps/sand/tools/block_size_sweep.ps1 - see that
    script's own comment for the shared reasoning.

    Candidate range is bounded above by ~9984: past that,
    test_plan_run_rejects_a_split_over_the_gather_budget's own two-marks-
    in-one-4-cell-run construction (test/suites/suite_gfx_dirty.c) stops
    fitting on the real 368x64 screen - its own _Static_assert catches
    this at compile time, so a value at or past that ceiling fails the
    host build outright rather than silently testing nothing.

    See docs/Notes/Display-and-Rendering.md's "The cap sweeps" section for
    what this found (8192, the shipped default, already sits on the right
    side of the reject/accept line for the case measured).

.PARAMETER IdfExportPath
    Path to ESP-IDF's export.ps1. Defaults to this project's usual
    install location - override if yours differs.

.PARAMETER ComPort
    Serial port the device is on. Defaults to COM3.
#>
param(
    [string]$IdfExportPath = "C:\Espressif\esp-idf-v5.5\export.ps1",
    [string]$ComPort = "COM3"
)

& $IdfExportPath | Out-Null

$launcher  = Resolve-Path "$PSScriptRoot\..\.."
$gfxDirtyH = "$launcher\main\gfx_dirty.h"
$results   = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force -Path $results | Out-Null
$captureScript = "$PSScriptRoot\capture_selftest.py"

function Write-Utf8NoBom($path, $content) {
    [System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding $false))
}

$originalGfxDirty = Get-Content $gfxDirtyH -Raw

# 8192 (128*64) is the shipped baseline. 4096/6144 are below it; 9216 is
# the highest value that still fits test_plan_run_rejects_a_split_over_
# the_gather_budget's compile-time safety margin.
$variants = @(4096, 6144, 8192, 9216)

$resultsPath = "$results\gather_pixels_sweep_results.csv"
"gather_max_pixels,selftest_failures,full_band_us,two_corners_us,two_marks_one_cell_us,near_budget_split_us" | Out-File -FilePath $resultsPath -Encoding utf8

Set-Location $launcher

try {
    foreach ($px in $variants) {
        Write-Host "=== GATHER_MAX_PIXELS=$px ==="

        $gfxContent = $originalGfxDirty -replace '#define GATHER_MAX_PIXELS \(128 \* 64\)', "#define GATHER_MAX_PIXELS ($px)"
        Write-Utf8NoBom $gfxDirtyH $gfxContent

        $hostOut = (& "C:\Program Files\Git\usr\bin\bash.exe" --login -c "test/run_tests.sh" 2>&1 | Out-String)
        if ($hostOut -notmatch "0 Failures") {
            Write-Host "HOST TESTS FAILED for GATHER_MAX_PIXELS=$px - skipping device run"
            Write-Host $hostOut
            "$px,HOST_TEST_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        idf.py -B build.diag build *>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "BUILD FAILED for GATHER_MAX_PIXELS=$px - skipping device run"
            "$px,BUILD_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        idf.py -B build.diag -p $ComPort flash *>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FLASH FAILED for GATHER_MAX_PIXELS=$px - skipping capture"
            "$px,FLASH_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        $captureFile = "$results\gather_pixels_capture_$px.txt"
        python $captureScript $captureFile --port $ComPort

        $text = Get-Content $captureFile -Raw

        $fails      = [regex]::Match($text, "SELFTEST_COMPLETE failures=(\d+)")
        $twoCorners = [regex]::Match($text, "present: full band (\d+) us, two \d+x\d+ corners (\d+) us")
        $twoInCell  = [regex]::Match($text, "present: full band \d+ us, two marks in one cell (\d+) us")
        $nearBudget = [regex]::Match($text, "present: full band \d+ us, near-budget split (\d+) us")

        $failCount     = if ($fails.Success)      { $fails.Groups[1].Value }      else { "MISSING" }
        $fullBand      = if ($twoCorners.Success)  { $twoCorners.Groups[1].Value } else { "MISSING" }
        $twoCornersUs  = if ($twoCorners.Success)  { $twoCorners.Groups[2].Value } else { "MISSING" }
        $twoInCellUs   = if ($twoInCell.Success)   { $twoInCell.Groups[1].Value }  else { "MISSING" }
        $nearBudgetUs  = if ($nearBudget.Success)  { $nearBudget.Groups[1].Value } else { "MISSING" }

        "$px,$failCount,$fullBand,$twoCornersUs,$twoInCellUs,$nearBudgetUs" | Add-Content -Path $resultsPath
        Write-Host "GATHER_MAX_PIXELS=$px -> failures=$failCount full_band=$fullBand two_corners=$twoCornersUs two_in_cell=$twoInCellUs near_budget=$nearBudgetUs"
    }
}
finally {
    Write-Utf8NoBom $gfxDirtyH $originalGfxDirty
    idf.py -B build.release build *>$null
    idf.py -B build.release -p $ComPort flash *>$null
    Write-Host "=== gfx_dirty.h restored, device reflashed with build.release from the original value ==="
}

Write-Host "=== DONE - results at $resultsPath ==="
Get-Content $resultsPath
