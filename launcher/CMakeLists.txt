cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(launcher)

# Static-RAM gate: predicts, from the just-linked map
# file, whether the framebuffer and then one sand grid still fit in the
# device's contiguous DRAM heap - the failure this catches previously
# only showed up as a boot hang or dozens of on-device "Expected Non-NULL"
# failures, never at build time. Runs for every variant (release, dev,
# diag) since a `project()` MACRO leaves its variables (${project_elf},
# the map path) visible after it returns. A failing build here is the
# intended outcome, not a bug in the gate - the message names the largest
# offenders and how to shrink them. See launcher/tools/check_static_ram.py
# for the full arithmetic.
idf_build_get_property(python PYTHON)
add_custom_command(TARGET ${project_elf} POST_BUILD
    COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/tools/check_static_ram.py --map ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
    COMMENT "static RAM gate: framebuffer + one grid must still fit (tools/check_static_ram.py)"
    VERBATIM)
