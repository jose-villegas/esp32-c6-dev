# The firmware proper. Test code is added only when CONFIG_LAUNCHER_SELFTEST is
# set, so a default build contains none of it - not the suites, not the runner,
# not the Unity framework.
set(app_srcs
    "main.c"
    "gfx.c"
    "touch.c"
    "touch_fsm.c"
    "gesture.c"
    "post.c"
    "ui_launcher.c"
    "apps/app_cube.c"
)
# post.c reaches for chip identity, flash size, I2C probing and the on-die
# temperature sensor, so it pulls in more of the platform than the rest.
set(app_reqs microui small3dlib esp_lcd heap esp_timer
             esp_driver_i2c esp_driver_tsens esp_hw_support spi_flash)

if(CONFIG_LAUNCHER_SELFTEST)
    # The suites are shared with the host runner - same sources, so what passes
    # on a laptop is the same set of assertions the board makes.
    list(APPEND app_srcs
        "selftest.c"
        "../test/suites/suite_touch_fsm.c"
        "../test/suites/suite_gesture.c"
        "../test/suites/suite_gfx.c"
    )
    list(APPEND app_reqs unity)
endif()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "."
    REQUIRES ${app_reqs}
)

if(CONFIG_LAUNCHER_SELFTEST)
    # Tells the shared suite headers they are being built for the target, which
    # is what enables the hardware-only suites.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BUILD=1)
endif()
