@echo off
REM Double-click launcher for fix-audited-code.sh --pool free.
REM
REM Runs the free-tier fixer pool (ollama-cloud/deepseek/gemini/nvidia free
REM tiers, local as final fallback) reviewed by deepseek/deepseek-v4-pro
REM (free-tier, code-focused, GA -- see the conversation that picked it over
REM a "preview" Gemini build), inside an isolated git worktree so this never
REM touches whatever's currently checked out or in progress in this folder.
REM Uses fix-audited-code.sh's own defaults otherwise: build.dev,
REM */apps/sand/* file_filter -- edit REVIEW_MODEL/EXTRA_ARGS below to widen
REM scope (e.g. --exclude main/apps/ combined with a "*" file_filter for the
REM whole project) or point at a different reviewer.
REM
REM Real free-tier model calls happen the moment this window opens -- no
REM confirmation prompt, since double-clicking it IS the confirmation. If
REM anything gets fixed, a branch is pushed and a PR compare link is
REM printed; if nothing does, no trace is left (see fix-audited-code.sh's
REM own --worktree cleanup behavior).

setlocal

set "REVIEW_MODEL=deepseek/deepseek-v4-pro"
set "EXTRA_ARGS="

cd /d "%~dp0"
cd ..

where bash >nul 2>nul
if %ERRORLEVEL%==0 (
    set "BASH_EXE=bash"
) else if exist "%ProgramFiles%\Git\bin\bash.exe" (
    set "BASH_EXE=%ProgramFiles%\Git\bin\bash.exe"
) else if exist "%ProgramFiles(x86)%\Git\bin\bash.exe" (
    set "BASH_EXE=%ProgramFiles(x86)%\Git\bin\bash.exe"
) else (
    echo Could not find Git Bash ^(bash.exe^) on PATH or in the usual
    echo Git for Windows install locations. Install Git for Windows first:
    echo   https://git-scm.com/download/win
    pause
    exit /b 1
)

echo ============================================================
echo  Free-tier code audit  --  fix-audited-code.sh --pool free
echo  Reviewer: %REVIEW_MODEL%
echo  Isolated worktree -- your current checkout is not touched.
echo ============================================================
echo.

"%BASH_EXE%" -lc "scripts/fix-audited-code.sh --review %REVIEW_MODEL% --pool free --worktree %EXTRA_ARGS%"

echo.
echo ==== finished, exit code %ERRORLEVEL% ====
pause
