/*=============================================================================
 * Per-test wall time, without editing a single suite file.
 *
 * Every suite calls Unity's RUN_TEST(func) directly. RUN_TEST is a macro
 * defined in unity_internals.h behind "#ifndef RUN_TEST" - if it is already
 * defined by the time that header is processed, Unity quietly steps aside
 * (that is what UNITY_SKIP_DEFAULT_RUNNER exists for). Forcing this header
 * in ahead of a suite's own "#include unity.h" - via the build's -include
 * flag, see main/CMakeLists.txt and test/run_tests.sh - makes that happen
 * for every suite at once, including the one (suite_sand.c) this project is
 * not free to edit right now, and the two dozen others not worth touching
 * just for this.
 *
 * Do NOT force this onto framework/unity.c's own compilation: that trips
 * the same guard from the other side and compiles UnityDefaultTestRun's
 * body out entirely (it becomes "the replacement runner"), which breaks the
 * only function this file's .c half calls. Both build scripts keep it out.
 *===========================================================================*/
#pragma once

#define RUN_TEST(func) suite_run_test_timed(func, #func, __LINE__)

/* Runs the test exactly as RUN_TEST always has (same file:line:name:PASS
 * line, byte for byte - see timing.c), then logs a separate line with how
 * long it took. */
void suite_run_test_timed(void (*func)(void), const char *name, int line);
