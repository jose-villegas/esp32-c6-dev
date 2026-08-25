@echo off
REM Run an ESP-IDF command with the environment it needs, and nothing else.
REM
REM   IDF_SHIM_DIR=<dir> IDF_SHIM_EXPORT=<export.bat> \
REM       cmd //c idf_shim.bat <command> [args...]
REM
REM This file exists for exactly one reason: ESP-IDF's idf_tools.py refuses to
REM run if MSYSTEM is set, and Git Bash always sets it. Bash cannot unset it
REM for a child - the MSYS runtime re-injects it, so even `env -u MSYSTEM`
REM arrives with MSYSTEM=MINGW64 intact (measured, not assumed). cmd's
REM `set VAR=` genuinely deletes it, and that is the whole trick.
REM
REM Two details that are easy to get wrong, both found by measuring:
REM
REM   - A .bat file, not an inline `cmd /c "..."`. Inline, cmd strips only the
REM     outermost quote pair, so `-D "SDKCONFIG_DEFAULTS=a;b"` arrives with its
REM     quotes still attached and poisons the CMake value. Through %% * from a
REM     batch file, bash's own argv survives intact - semicolons and all.
REM
REM   - The directory and export path come in as ENVIRONMENT variables rather
REM     than as leading arguments, because `shift` does not affect %% * in cmd:
REM     it still expands to the original list, so consuming leading arguments
REM     that way is impossible and silently re-runs the first one as a command.
REM
REM Everything else - which commands to run, in what order, what to do when one
REM fails - stays in the calling script, in POSIX sh. This is the only
REM Windows-shaped file in the tooling.

set MSYSTEM=
cd /d "%IDF_SHIM_DIR%" || exit /b 1
call "%IDF_SHIM_EXPORT%" >nul 2>&1 || exit /b 1
%*
