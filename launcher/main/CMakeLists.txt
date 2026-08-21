# DEVICE_BUILD tells the shared test suites they are being compiled for the
# target, which enables the hardware-only ones.
idf_component_register(
    SRCS "main.c" "gfx.c" "touch.c" "touch_fsm.c" "gesture.c"
         "ui_launcher.c" "apps/app_cube.c"
         # The self test and its suites ship inside the binary on purpose:
         # what runs at boot must be the same code that ships, not a
         # separately built test image.
         "selftest.c"
         "../test/suites/suite_touch_fsm.c"
         "../test/suites/suite_gesture.c"
         "../test/suites/suite_gfx.c"
    INCLUDE_DIRS "."
    REQUIRES microui small3dlib esp_lcd heap esp_timer unity
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BUILD=1)
