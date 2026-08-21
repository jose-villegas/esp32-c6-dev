idf_component_register(
    SRCS "main.c" "gfx.c" "touch.c" "touch_fsm.c" "gesture.c"
         "ui_launcher.c" "apps/app_cube.c"
    INCLUDE_DIRS "."
    REQUIRES microui small3dlib esp_lcd heap esp_timer
)
