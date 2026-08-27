<#
.SYNOPSIS
    Sweeps ROW_MAX_RUNS (main/apps/sand/row_runs.h) and LEAF_REFINE_MAX_RUNS
    (main/gfx_dirty.h) together - they mirror each other by design.

.DESCRIPTION
    Both cap how many separate dirty regions get tracked/gathered per row
    before falling back to a coarser send. Same build+flash+measure+
    restore pattern as main/apps/sand/tools/block_size_sweep.ps1 - see that
    script's own comment for the shared reasoning, and
    docs/Notes/Display-and-Rendering.md's "The cap sweeps" section for
    what this one found (raising the cap regressed the case it exists to
    help - both stay at 2 as shipped).

    Stays in shared tools/sweeps/ rather than moving into
    main/apps/sand/tools/ alongside block_size_sweep.ps1, even though it
    touches a sand header: it sweeps ROW_MAX_RUNS (main/apps/sand/row_runs.h)
    and LEAF_REFINE_MAX_RUNS (main/gfx_dirty.h, a shell-owned file with no
    app) together, by design - the two mirror each other. Filing this under
    the sand app would make the gfx-dirty half of the sweep vanish if the
    sand app folder were ever deleted, which is the same "nothing left
    dangling" promise violated in the other direction.

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
$rowRunsH  = "$launcher\main\apps\sand\row_runs.h"
$gfxDirtyH = "$launcher\main\gfx_dirty.h"
$results   = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force -Path $results | Out-Null
$captureScript = "$PSScriptRoot\capture_selftest.py"

function Write-Utf8NoBom($path, $content) {
    [System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding $false))
}

$originalRowRuns  = Get-Content $rowRunsH -Raw
$originalGfxDirty = Get-Content $gfxDirtyH -Raw

# 2 is the shipped baseline.
$variants = @(2, 3, 4)

$resultsPath = "$results\row_leaf_sweep_results.csv"
"cap,selftest_failures,full_band_us,two_corners_us,three_marks_us,two_marks_one_cell_us" | Out-File -FilePath $resultsPath -Encoding utf8

Set-Location $launcher

try {
    foreach ($cap in $variants) {
        Write-Host "=== ROW_MAX_RUNS=LEAF_REFINE_MAX_RUNS=$cap ==="

        $rowContent = $originalRowRuns -replace '#define ROW_MAX_RUNS \d+', "#define ROW_MAX_RUNS $cap"
        Write-Utf8NoBom $rowRunsH $rowContent

        $gfxContent = $originalGfxDirty -replace '#define LEAF_REFINE_MAX_RUNS \d+', "#define LEAF_REFINE_MAX_RUNS $cap"
        Write-Utf8NoBom $gfxDirtyH $gfxContent

        $hostOut = (& "C:\Program Files\Git\usr\bin\bash.exe" --login -c "test/run_tests.sh" 2>&1 | Out-String)
        if ($hostOut -notmatch "0 Failures") {
            Write-Host "HOST TESTS FAILED for cap=$cap - skipping device run"
            Write-Host $hostOut
            "$cap,HOST_TEST_FAILURE,,,," | Add-Content -Path $resultsPath
            continue
        }

        idf.py -B build.diag build *>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "BUILD FAILED for cap=$cap - skipping device run"
            "$cap,BUILD_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        idf.py -B build.diag -p $ComPort flash *>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FLASH FAILED for cap=$cap - skipping capture"
            "$cap,FLASH_FAILURE,,,,," | Add-Content -Path $resultsPath
            continue
        }

        $captureFile = "$results\row_leaf_capture_$cap.txt"
        python $captureScript $captureFile --port $ComPort

        $text = Get-Content $captureFile -Raw

        $fails       = [regex]::Match($text, "SELFTEST_COMPLETE failures=(\d+)")
        $twoCorners  = [regex]::Match($text, "present: full band (\d+) us, two \d+x\d+ corners (\d+) us")
        $threeMarks  = [regex]::Match($text, "present: full band (\d+) us, three \d+x\d+ marks (\d+) us")
        $twoInCell   = [regex]::Match($text, "present: full band \d+ us, two marks in one cell (\d+) us")

        $failCount    = if ($fails.Success)      { $fails.Groups[1].Value }      else { "MISSING" }
        $fullBand     = if ($twoCorners.Success)  { $twoCorners.Groups[1].Value } else { "MISSING" }
        $twoCornersUs = if ($twoCorners.Success)  { $twoCorners.Groups[2].Value } else { "MISSING" }
        $threeMarksUs = if ($threeMarks.Success)  { $threeMarks.Groups[2].Value } else { "MISSING" }
        $twoInCellUs  = if ($twoInCell.Success)   { $twoInCell.Groups[1].Value }  else { "MISSING" }

        "$cap,$failCount,$fullBand,$twoCornersUs,$threeMarksUs,$twoInCellUs" | Add-Content -Path $resultsPath
        Write-Host "cap=$cap -> failures=$failCount full_band=$fullBand two_corners=$twoCornersUs three_marks=$threeMarksUs two_in_cell=$twoInCellUs"
    }
}
finally {
    Write-Utf8NoBom $rowRunsH $originalRowRuns
    Write-Utf8NoBom $gfxDirtyH $originalGfxDirty
    idf.py -B build.release build *>$null
    idf.py -B build.release -p $ComPort flash *>$null
    Write-Host "=== row_runs.h/gfx_dirty.h restored, device reflashed with build.release from the original values ==="
}

Write-Host "=== DONE - results at $resultsPath ==="
Get-Content $resultsPath
