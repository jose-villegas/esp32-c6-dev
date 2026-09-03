# The shell and the board drivers. Apps are NOT listed here - see below.
set(app_srcs
    "main.c"
    "boot/boot_anim.c"
    "boot/post.c"
    "boot/post_ui.c"
    "display/display.c"
    "gfx/gfx.c"
    "gfx/icons.c"
    "input/touch.c"
    "input/touch_fsm.c"
    "input/button_fsm.c"
    "input/buttons.c"
    "input/gesture.c"
    "input/imu.c"
    "ui/ui.c"
    "ui/ui_launcher.c"
    "util/device_state.c"
    "util/screenshot.c"
)

# Apps are discovered, not listed.
#
# Everything an app owns lives in main/apps/<name>/ - its entry point, any
# logic of its own, and its test suite. Adding an app means creating the
# folder; deleting one means deleting the folder. Neither touches this file,
# and neither touches main.c, because apps register themselves (see app.h).
#
# CONFIGURE_DEPENDS makes CMake re-glob when the folder contents change, so a
# new app is picked up without a manual reconfigure.
#
# It cannot be used unconditionally. ESP-IDF evaluates this file twice: once in
# script mode (cmake -P) to work out component requirements, and once for real.
# CONFIGURE_DEPENDS is illegal in script mode and hard-errors the configure.
# The script pass only cares about REQUIRES, so plain globbing is fine there.
# (This is the same early pass that makes a Kconfig-gated REQUIRES silently
# fail - see the note on unity below.)
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE discovered_apps "${CMAKE_CURRENT_LIST_DIR}/apps/*.c")
else()
    file(GLOB_RECURSE discovered_apps CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_LIST_DIR}/apps/*.c")
endif()

# apps/<name>/tools/ holds that app's own host-side tooling (sweep scripts,
# report generators) - see docs/Launcher-Architecture.md, "An app is a
# folder". It is not firmware. But the glob above is apps/*.c, recursive, so
# a future .c placed under one of those tools/ folders (a small host-side
# helper program, say) would be swept in right alongside the app's real
# sources - and then WHOLE_ARCHIVE, below, force-links it into the image
# regardless of whether anything references it, so --gc-sections never gets
# the chance to drop it. Excluded by folder rather than by name, same as the
# diagnostics rule below, so the rule is structural rather than needing a
# per-tool update. Nothing lives here yet; this guard goes in with the
# convention itself, because the failure it prevents - a host tool's main()
# silently linked into device firmware - is silent and baffling to debug
# after the fact.
#
# Filtered HERE, before the suite split below, and not after it:
# app_suites is captured from discovered_apps and would otherwise keep
# a suite_*.c living under a tools/ folder, putting it back into the
# selftest image through the other door.
list(FILTER discovered_apps EXCLUDE REGEX "/apps/[^/]*/tools/")

# Test suites are app-owned but must not ship. They are added back below when
# CONFIG_LAUNCHER_SELFTEST is set.
set(app_suites ${discovered_apps})
list(FILTER app_suites INCLUDE REGEX "/suite_[^/]*\\.c$")
list(FILTER discovered_apps EXCLUDE REGEX "/suite_[^/]*\\.c$")

# Diagnostics carries the gfx debug-overlay checkboxes a developer wants
# while working on sand, plus the POST report and (SELFTEST builds only -
# see app_diagnostics.c) the self-test runner button. None of that belongs
# in a release image: entering the app re-runs POST, which cycles the audio
# rail and drops the display off SPI2 mid-session - reasonable while
# debugging, not something to leave reachable in a shipped product. Gated
# on DEVELOPMENT rather than SELFTEST so it ships in a --dev build too, with
# no test suites along for the ride - that is the whole reason a --dev
# build exists: SELFTEST's linked-in suites cost enough static RAM that
# sand's grid allocation can fail. LAUNCHER_SELFTEST `select`s
# LAUNCHER_DEVELOPMENT (Kconfig.projbuild), so this one condition still
# covers both --dev and --diag. Excluded by folder rather than by name so
# the rule is structural.
if(NOT CONFIG_LAUNCHER_DEVELOPMENT)
    list(FILTER discovered_apps EXCLUDE REGEX "/apps/diagnostics/")
endif()

list(APPEND app_srcs ${discovered_apps})

# post.c reaches for chip identity, flash size, I2C probing and the on-die
# temperature sensor, so it pulls in more of the platform than the rest.
set(app_reqs microui small3dlib esp_lcd heap esp_timer
             esp_driver_i2c esp_driver_tsens esp_hw_support spi_flash
             fatfs esp_driver_sdspi sdmmc
             esp_driver_usb_serial_jtag vfs
             waveshare__esp_lcd_sh8601
             # Unity is listed unconditionally, NOT inside the block below.
             # ESP-IDF expands REQUIRES in an early pass where CONFIG_* is not
             # yet defined, so a Kconfig-gated REQUIRES silently does nothing -
             # SRCS and compile definitions are evaluated later and do work,
             # which makes the failure look baffling: the test sources compile
             # but cannot find unity.h.
             #
             # This costs release nothing. IDF puts unity in the component graph
             # either way; REQUIRES only decides whether main can see its
             # headers. With no test sources compiled, nothing references it and
             # --gc-sections drops it from the image.
             unity)

if(CONFIG_LAUNCHER_SELFTEST)
    # The suites are shared with the host runner - same sources, so what passes
    # on a laptop is the same set of assertions the board makes.
    set(suite_srcs
        "../test/suites/suite_touch_fsm.c"
        "../test/suites/suite_gesture.c"
        "../test/suites/suite_button_fsm.c"
        "../test/suites/suite_rng.c"
        "../test/suites/suite_boot_anim.c"
        "../test/suites/suite_boot_anim_perf.c"
        "../test/suites/suite_fixed.c"
        "../test/suites/suite_tween.c"
        "../test/suites/suite_gfx.c"
        "../test/suites/suite_gfx_dirty.c"
        "../test/suites/suite_gfx_color.c"
        "../test/suites/suite_gfx_font.c"
        "../test/suites/suite_icons.c"
        "../test/suites/suite_ui.c"
        "../test/suites/suite_ui_style.c"
        "../test/suites/suite_ui_transform.c"
        "../test/suites/suite_ui_centered_rect.c"
        "../test/suites/suite_display.c"
        "../test/suites/suite_screenshot.c"
        "../test/suites/suite_device_state.c"
    )
    list(APPEND app_srcs
        "boot/selftest.c"
        "../test/suites.c"
        "../test/timing.c"
        ${suite_srcs}
        ${app_suites}
    )

    # Every suite calls RUN_TEST(func) directly; timing.h intercepts that
    # macro (see its own comment) to log a wall-clock line per test without
    # editing a single suite file. -include forces it in ahead of each
    # suite's own "#include unity.h", which is what lets Unity's own
    # "#ifndef RUN_TEST" step aside for it. Scoped to just the suite files -
    # forcing it onto every source in this component would be harmless too
    # (nothing else calls RUN_TEST), but there's no reason to.
    set_source_files_properties(${suite_srcs} ${app_suites} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/../test/timing.h")
endif()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "." "../test"
    REQUIRES ${app_reqs}

    # Required by self-registration, and easy to get wrong.
    #
    # This component becomes libmain.a, and a linker only extracts an archive
    # member if it resolves an undefined symbol. Nothing references an app by
    # name any more - that is the entire point - so app_sand.o would never be
    # extracted, its constructor would never enter .init_array, and the app
    # would silently vanish from the menu. Not a link error: a smaller binary
    # and a shorter list.
    #
    # WHOLE_ARCHIVE links every member regardless. Section garbage collection
    # still runs, so genuinely unreachable code is still dropped.
    WHOLE_ARCHIVE
)

if(CONFIG_LAUNCHER_SELFTEST)
    # Tells the shared suite headers they are being built for the target, which
    # is what enables the hardware-only suites.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BUILD=1)
endif()
