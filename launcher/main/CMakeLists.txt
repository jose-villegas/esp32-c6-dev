# The shell and the board drivers. Apps are NOT listed here - see below.
set(app_srcs
    "main.c"
    "gfx.c"
    "icons.c"
    "touch.c"
    "touch_fsm.c"
    "button_fsm.c"
    "buttons.c"
    "gesture.c"
    "imu.c"
    "post.c"
    "post_ui.c"
    "ui.c"
    "ui_launcher.c"
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

# Diagnostics is a bench tool, not a feature: it re-runs POST, which cycles the
# audio rail and drops the display off SPI2 mid-session. Excluded by folder
# rather than by name so the rule is structural.
if(NOT CONFIG_LAUNCHER_SELFTEST)
    list(FILTER discovered_apps EXCLUDE REGEX "/apps/diagnostics/")
endif()

list(APPEND app_srcs ${discovered_apps})

# post.c reaches for chip identity, flash size, I2C probing and the on-die
# temperature sensor, so it pulls in more of the platform than the rest.
set(app_reqs microui small3dlib esp_lcd heap esp_timer
             esp_driver_i2c esp_driver_tsens esp_hw_support spi_flash
             fatfs esp_driver_sdspi sdmmc
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
    list(APPEND app_srcs
        "selftest.c"
        "../test/suites.c"
        "../test/suites/suite_touch_fsm.c"
        "../test/suites/suite_gesture.c"
        "../test/suites/suite_button_fsm.c"
        "../test/suites/suite_rng.c"
        "../test/suites/suite_gfx.c"
        "../test/suites/suite_gfx_dirty.c"
        "../test/suites/suite_gfx_color.c"
        "../test/suites/suite_icons.c"
        "../test/suites/suite_ui.c"
        ${app_suites}
    )
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
