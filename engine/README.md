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
- `engine_runtime` compiles the firmware's real launcher UI, Microui bridge
  and `gfx.c` directly; it is a host target, never an ESP-IDF component.
- A C interface supplies exact 448 x 368 and 368 x 448 framebuffers.
- The workspace docks hierarchy left, preview center, inspector right and
  problems below; later adjustments persist in Dear ImGui's settings.
- Both preview textures contain the current firmware launcher rendered through
  Microui, `ui.c` and `gfx.c`, then read back from the real framebuffer.
- The hierarchy exposes stable launcher element IDs. Selecting one outlines it
  in both orientations, while the inspector edits its active-orientation
  rectangle and refreshes both previews immediately.

The launcher's orientation-specific geometry is authored in
`launcher/main/ui/launcher_layout.json` and deterministically baked into
`launcher_layout_generated.h`. The checked-in header is the only form used by
firmware; the device does not parse JSON or run a layout solver.

The editor loads that JSON into a typed C++ document and validates canvas
bounds, minimum card targets, card overlap and page-indicator placement. Save
and Ctrl+S update the source JSON only when it is valid. Rebaking the generated
firmware header remains an explicit, separate step, so preview edits cannot
silently change a device build.

Dependencies are fetched into the untracked build directory rather than
vendored into firmware source.

## Build

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Debug
cmake --build engine/build --config Debug
ctest --test-dir engine/build --output-on-failure
```

The executable is named `engine`.
