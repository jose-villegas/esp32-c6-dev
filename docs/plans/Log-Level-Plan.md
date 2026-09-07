# Plan: a compile-time log-level ceiling per build variant, not a logging class

**Status**: planned, not built. Written 2026-08-30, out of the conversation
that gated `report_fps()` behind `CONFIG_LAUNCHER_DEVELOPMENT`
(`launcher/main/main.c`) and then asked whether a unified logging
abstraction would catch this class of bug earlier.

---

## What today's fix didn't generalize

`report_fps()`'s `ESP_LOGI` needed a hand-written `#if CONFIG_LAUNCHER_DEVELOPMENT`
because nothing structural was keeping it out of release. `CONFIG_LAUNCHER_DEVELOPMENT`
gates whole dev-only *subsystems* (the screenshot listener, `device_state`,
`app_sand.c`'s frame-timing averages) — it says nothing about log
*severity*. Every other `ESP_LOGI`/`ESP_LOGW` call site in the tree (48 and 9
respectively, as of this writing) ships in release today unless someone
remembers to wrap it by hand. That's the same bug class as `report_fps()`,
just not yet tripped over again.

## The idiom already in ESP-IDF, verified not assumed

`ESP_LOGI`/`W`/`E`/`D`/`V` expand (`components/log/include/esp_log.h`) to:

```c
if (LOG_LOCAL_LEVEL >= level) { ESP_LOG_LEVEL(level, tag, format, ...); }
```

`LOG_LOCAL_LEVEL` is `CONFIG_LOG_MAXIMUM_LEVEL` — a Kconfig integer, i.e. a
genuine compile-time constant, not a runtime flag. Confirmed by diffing the
compiled object, not by reading the header and hoping:

- Building `main.c.obj` twice — once at the project's current ceiling
  (`CONFIG_LOG_MAXIMUM_LEVEL=3`, INFO) and once in a scratch build forced to
  `CONFIG_LOG_DEFAULT_LEVEL_ERROR` + `CONFIG_LOG_MAXIMUM_LEVEL_ERROR` — shows
  the `"%.1f fps"` format-string literal present in the first `.o` and absent
  from the second. The compiler is genuinely deleting the call and its
  string, not just skipping it at runtime.
- Lowering the ceiling changed *unrelated* codegen: see the `post.c` finding
  below. That side effect is itself proof the branch is really gone at
  compile time — removing dead code changed what GCC chose to inline
  elsewhere in the same translation unit.

## Why not a custom logging class

The instinct behind asking is right to be suspicious of a hand-rolled
wrapper (`log_info(fmt, ...)` that no-ops its body in release): a function
call passing a format string as a pointer argument does not reliably get
that argument and the call itself eliminated without whole-program
optimization (LTO) across translation units, which this project does not
enable. The compiler can fold away `if (constant-expr)` *in the same
translation unit as the call site* — which is exactly what
`ESP_LOG_LEVEL_LOCAL` already does — but it cannot generally prove a
variadic argument passed to an external-looking function is dead. Building
a wrapper would mean reimplementing, in a form the compiler can't reliably
strip, a mechanism the SDK already provides in a form it can.

So "a unified logger that speaks this system's language" should mean a
shared **policy** — a Kconfig-driven severity ceiling, plus a documented
convention for which macro a given line should use — not a shared
**object**. Call `ESP_LOGE`/`W`/`I`/`D`/`V` directly, correctly classified;
let Kconfig decide what survives into which build.

## A Kconfig coupling worth knowing before touching this

`LOG_MAXIMUM_LEVEL` cannot be set below `LOG_DEFAULT_LEVEL` —
`components/log/Kconfig.level` gates `LOG_MAXIMUM_LEVEL_ERROR` behind
`depends on LOG_DEFAULT_LEVEL < 1`, and so on per level. The ceiling and the
runtime default floor move together; setting only one silently no-ops
(confirmed by trying it — the Kconfig choice falls back to
`LOG_MAXIMUM_EQUALS_DEFAULT`).

## The plan

1. **Audit the existing 48 `ESP_LOGI` + 9 `ESP_LOGW` call sites** before
   touching the ceiling. Most are probably legitimate dev-only noise
   (correct to lose in release), but some may be miscategorized and belong
   at `ESP_LOGW`/`ESP_LOGE` instead of silently disappearing.
2. **Release** (`sdkconfig` / `sdkconfig.defaults`):
   `CONFIG_LOG_DEFAULT_LEVEL_ERROR=y` + `CONFIG_LOG_MAXIMUM_LEVEL_ERROR=y`
   (or the `_WARN` variants if warnings should compile in but stay quiet by
   default — see open questions).
3. **Dev / diag** (`sdkconfig.defaults.dev` / `.diag`): explicit
   `CONFIG_LOG_DEFAULT_LEVEL_VERBOSE=y` + `CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y`,
   mirroring how `CONFIG_LAUNCHER_DEVELOPMENT` is already split per variant.
4. **Full clean rebuild of both variants under the project's normal
   `-Werror`, plus `./launcher/test/run_tests.sh`**, after flipping the
   ceiling — not a config-only, zero-risk toggle. See the `post.c` finding:
   removing dead log calls changes inlining decisions and can expose latent
   warnings in code that has nothing to do with logging.
5. **Verify with `nm`/string-diffing**, the same "verified rather than
   assumed" convention `Testing-Guide.md`'s SELFTEST/DEVELOPMENT section
   already uses.
6. **Document the policy in `Testing-Guide.md`**, alongside "Development-only
   instrumentation is its own flag, not SELFTEST": `CONFIG_LAUNCHER_DEVELOPMENT`
   gates whole dev-only subsystems; `CONFIG_LOG_MAXIMUM_LEVEL` gates log
   severity project-wide. A bare log line inside an otherwise-always-on
   function should just be the correctly-chosen `ESP_LOG*` macro going
   forward, not a manual `#if`.
7. **Re-examine existing hand-rolled `#if CONFIG_LAUNCHER_DEVELOPMENT`
   guards that wrap nothing but a log call** (as opposed to real
   accounting work, like `app_sand.c`'s frame-timing averages) — some of
   those become redundant once the ceiling does the job, but that needs a
   case-by-case look, not a blanket removal.

## Found in passing: a latent truncation bug this plan would expose

`main/boot/post.c:57`:

```c
snprintf(r->detail, sizeof(r->detail), "%s", detail ? detail : "");
```

`r->detail` is a 72-byte buffer. This compiles clean today because nothing
inlines `report()` (`main/boot/post.c`) into its callers. Forcing the log
ceiling down in a scratch build shrank `report()` enough — by deleting its
own `ESP_LOGI` call — that GCC inlined it into `check_sdcard_live()` and
`post_rerun()`, at which point `-Werror=format-truncation` correctly flags
that one caller's `detail` source can be up to 79 bytes. The bug is real and
pre-existing, independent of logging; it was just never visible before
because nothing triggered that inlining. **This needs its own small fix
(widen `r->detail`, or bound the source strings) before step 4 above can
land cleanly** — it is not something to bundle into the log-level change
itself.

## Open questions to settle when this is actually picked up

- **Error-only, or error+warn, in release?** "Warnings being optional" from
  the conversation that prompted this leans toward compiling `ESP_LOGW` in
  (so a field unit could be bumped to WARN via `esp_log_level_set()` at
  runtime without a reflash) while keeping the *default* active level at
  ERROR. That is exactly what `CONFIG_LOG_MAXIMUM_LEVEL_WARN` +
  `CONFIG_LOG_DEFAULT_LEVEL_ERROR` gives.
- Does every existing `#if CONFIG_LAUNCHER_DEVELOPMENT`-gated pure log line
  become redundant, or are there cases worth keeping doubly-gated on
  purpose?

## Non-goals

Nothing here is being built now, including the `post.c` fix, which is
tracked separately. This is a placeholder for a future session.

## Related

- [Testing-Guide.md](Testing-Guide.md) — "Development-only instrumentation
  is its own flag, not SELFTEST": the rule this plan complements rather than
  replaces.
