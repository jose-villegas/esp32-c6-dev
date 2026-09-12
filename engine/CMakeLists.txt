cmake_minimum_required(VERSION 3.24)

project(engine LANGUAGES C CXX)

include(CTest)

find_package(Python3 COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    engine_imgui
    engine_runtime
    ${ENGINE_SDL_TARGET}
)
target_compile_definitions(engine PRIVATE
    ENGINE_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

if(MSVC)
    target_compile_options(engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(engine_runtime_smoke tests/runtime_smoke.c)
    target_link_libraries(engine_runtime_smoke PRIVATE engine_runtime)
    add_test(NAME engine_runtime_smoke COMMAND engine_runtime_smoke)

    add_executable(engine_launcher_document_test tests/launcher_document_test.cpp)
    target_link_libraries(engine_launcher_document_test PRIVATE engine_document)
    target_compile_definitions(engine_launcher_document_test PRIVATE
        ENGINE_TEST_LAYOUT_PATH="${LAUNCHER_DIR}/main/ui/launcher_layout.json"
    )
    add_test(NAME engine_launcher_document COMMAND engine_launcher_document_test)
    if(Python3_Interpreter_FOUND)
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
