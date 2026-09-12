cmake_minimum_required(VERSION 3.24)

project(engine LANGUAGES C CXX)

include(CTest)

find_package(Python3 COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENGINE_ENABLE_COVERAGE "Instrument Engine-owned host targets for coverage" OFF)

include(FetchContent)

set(LAUNCHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../launcher")

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL2)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Docking is maintained as a long-lived upstream branch. Pin it once the
# editor's first vertical slice establishes the API surface we actually use.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

if(TARGET SDL2::SDL2-static)
    set(ENGINE_SDL_TARGET SDL2::SDL2-static)
else()
    set(ENGINE_SDL_TARGET SDL2::SDL2)
endif()

add_library(engine_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(engine_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(engine_imgui PUBLIC ${ENGINE_SDL_TARGET})

add_library(engine_editor_core STATIC
    editor/core/dockspace.cpp
    editor/core/rgb565_texture.cpp
)
target_include_directories(engine_editor_core PUBLIC editor)
target_link_libraries(engine_editor_core PUBLIC
    engine_imgui
    ${ENGINE_SDL_TARGET}
)

if(MSVC)
    target_compile_options(engine_editor_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(engine_editor_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(engine_runtime STATIC
    runtime/runtime.c
    ${LAUNCHER_DIR}/main/gfx/gfx.c
    ${LAUNCHER_DIR}/main/ui/ui.c
    ${LAUNCHER_DIR}/main/ui/ui_launcher.c
    ${LAUNCHER_DIR}/main/ui/ui_pointer.c
    ${LAUNCHER_DIR}/components/microui/src/microui.c
)
target_include_directories(engine_runtime
    PUBLIC include
    PRIVATE
        runtime/host
        ${LAUNCHER_DIR}/main
        ${LAUNCHER_DIR}/components/microui/include
)
target_compile_definitions(engine_runtime PRIVATE
    CONFIG_LAUNCHER_DEVELOPMENT=0
)

if(MSVC)
    target_compile_options(engine_runtime PRIVATE /W4)
else()
    # The device-only presentation statics are intentionally unused by a
    # host build. The atlas stores byte masks in char rows, so signed-char
    # hosts also warn on the intentional 0xff values.
    target_compile_options(engine_runtime PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-overflow
        -Wno-unused-function
        -Wno-unused-parameter
        -Wno-unused-variable
    )
endif()

add_library(engine_document STATIC editor/launcher_document.cpp)
target_include_directories(engine_document PUBLIC editor)
target_link_libraries(engine_document PUBLIC nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(engine_document PRIVATE /W4 /permissive-)
else()
    target_compile_options(engine_document PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(engine
    editor/main.cpp
    preview/preview_surface.c
)
target_include_directories(engine PRIVATE include)
target_link_libraries(engine PRIVATE
    engine_document
    engine_editor_core
    engine_imgui
    engine_runtime
    ${ENGINE_SDL_TARGET}
)
target_compile_definitions(engine PRIVATE
    ENGINE_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(Python3_Interpreter_FOUND)
    target_compile_definitions(engine PRIVATE
        ENGINE_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
    )
endif()

if(MSVC)
    target_compile_options(engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    include(GoogleTest)

    add_executable(engine_runtime_smoke tests/runtime_smoke.c)
    target_link_libraries(engine_runtime_smoke PRIVATE engine_runtime)
    add_test(NAME engine_runtime_smoke COMMAND engine_runtime_smoke)

    add_executable(engine_launcher_document_test tests/launcher_document_test.cpp)
    target_link_libraries(engine_launcher_document_test PRIVATE engine_document GTest::gmock_main)
    target_compile_definitions(engine_launcher_document_test PRIVATE
        ENGINE_TEST_LAYOUT_PATH="${LAUNCHER_DIR}/main/ui/launcher_layout.json"
    )
    gtest_discover_tests(engine_launcher_document_test TEST_PREFIX "engine_document.")

    add_executable(engine_core_test tests/edit_history_test.cpp)
    target_include_directories(engine_core_test PRIVATE editor)
    target_link_libraries(engine_core_test PRIVATE GTest::gtest_main)
    gtest_discover_tests(engine_core_test TEST_PREFIX "engine_core.")

    if(Python3_Interpreter_FOUND)
        add_test(NAME engine_launcher_bake_command COMMAND engine --check-bake)
        add_test(
            NAME launcher_layout_codegen
            COMMAND ${Python3_EXECUTABLE}
                ${LAUNCHER_DIR}/tools/gen_launcher_layout.py
                ${LAUNCHER_DIR}/main/ui/launcher_layout.json
                ${LAUNCHER_DIR}/main/ui/launcher_layout_generated.h
                --check
        )
        add_test(
            NAME launcher_layout_generator
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_layout_generator_test.py
        )
    endif()
endif()

if(ENGINE_ENABLE_COVERAGE)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "ENGINE_ENABLE_COVERAGE requires BUILD_TESTING=ON")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENGINE_ENABLE_COVERAGE requires GCC or Clang")
    endif()
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "ENGINE_ENABLE_COVERAGE requires Python 3 with gcovr installed")
    endif()

    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import gcovr"
        RESULT_VARIABLE ENGINE_GCOVR_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT ENGINE_GCOVR_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Coverage requires gcovr. Install it with: ${Python3_EXECUTABLE} -m pip install gcovr==8.6"
        )
    endif()

    set(ENGINE_COVERAGE_TARGETS
        engine_document
        engine_runtime
        engine_launcher_document_test
        engine_core_test
        engine_runtime_smoke
    )
    foreach(target IN LISTS ENGINE_COVERAGE_TARGETS)
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
    endforeach()
    # The editor consumes instrumented static libraries but is not itself in
    # the coverage scope, so it only needs the gcov link runtime.
    target_link_options(engine PRIVATE --coverage)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        foreach(target IN LISTS ENGINE_COVERAGE_TARGETS)
            target_compile_options(${target} PRIVATE -fprofile-abs-path)
        endforeach()
    endif()

    add_custom_target(engine_coverage
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
        COMMAND ${Python3_EXECUTABLE} -m gcovr
            --root ${CMAKE_CURRENT_SOURCE_DIR}/..
            --gcov-object-directory ${CMAKE_BINARY_DIR}
            --filter engine/editor/launcher_document.cpp
            --filter engine/editor/core/edit_history.h
            --filter engine/runtime/runtime.c
            --exclude-throw-branches
            --exclude-unreachable-branches
            --delete
            --txt-summary
            --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
            --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
            --xml-pretty
            --fail-under-line 80
            --fail-under-branch 70
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        DEPENDS engine engine_launcher_document_test engine_core_test engine_runtime_smoke
        USES_TERMINAL
        COMMENT "Running tests and enforcing Engine coverage gates"
    )
endif()
