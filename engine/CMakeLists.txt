cmake_minimum_required(VERSION 3.24)

project(engine LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

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

add_executable(engine
    editor/main.cpp
    preview/preview_surface.c
)
target_include_directories(engine PRIVATE include)
target_link_libraries(engine PRIVATE engine_imgui ${ENGINE_SDL_TARGET})

if(MSVC)
    target_compile_options(engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
