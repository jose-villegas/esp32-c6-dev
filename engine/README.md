# Engine

This directory is the host-side root for the engine's authoring and preview
tools. UI layout editing is the first vertical slice, not the boundary of the
tool: later modules can add scene, level, animation, material and asset
authoring without renaming the application.

The device runtime remains under `launcher/`. SDL2 and Dear ImGui are host-only
dependencies and must never enter an ESP-IDF component graph.

## Current bootstrap

The first executable proves the durable boundary:

- Dear ImGui owns editor chrome, docking and host input.
- SDL2 owns the native window and RGB565 preview textures.
- `engine_runtime` compiles and initializes the firmware's real `gfx.c` and
  Microui sources directly; it is a host target, never an ESP-IDF component.
- A C interface supplies exact 448 x 368 and 368 x 448 framebuffers.
- The initial workspace docks hierarchy left, preview center, inspector right
  and problems below; later adjustments persist in Dear ImGui's settings.
- The temporary transport pattern is not a second renderer. Its only purpose
  is to prove pixels can cross the boundary before the real host `gfx.c` path
  replaces it.

Dependencies are fetched into the untracked build directory rather than
vendored into firmware source.

## Build

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Debug
cmake --build engine/build --config Debug
ctest --test-dir engine/build --output-on-failure
```

The executable is named `engine`. The next slice is to replace
`engine_preview_render_transport_test()` with pixels read from the initialized
firmware framebuffer, then load the authored layout data described in
`docs/plans/UI-Editor-Plan.md`.
