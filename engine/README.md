# Engine

This directory is the host-side root for the engine's authoring and preview
tools. UI layout editing is the first vertical slice, not the boundary of the
tool: later modules can add scene, level, animation, material and asset
authoring without renaming the application.

The device runtime remains under `launcher/`. SDL2 and Dear ImGui are host-only
dependencies and must never enter an ESP-IDF component graph.

## Current bootstrap

The first executable proves the durable boundary:

- `editor/core/` owns reusable host-editor services: dockspace setup,
  document history and RGB565 preview texture lifetime.
- Screen selection, geometry rules, validation and baking remain System
  Workspace behavior rather than leaking into that core.
- Dear ImGui owns editor chrome, docking and host input.
- SDL2 owns the native window and RGB565 preview textures.
- `engine_runtime` compiles the firmware's real launcher UI, Microui bridge
  and `gfx.c` directly; it is a host target, never an ESP-IDF component.
- A C interface supplies exact 448 x 368 and 368 x 448 framebuffers.
- The workspace docks hierarchy left, preview center, inspector right and
  problems below; later adjustments persist in Dear ImGui's settings.
- Both preview textures contain the selected firmware screen rendered through
  Microui, `ui.c` and `gfx.c`, then read back from the real framebuffer.
- The hierarchy exposes Launcher and Control Center as sibling system
  documents with stable element IDs. Selecting one outlines it in both
  orientations, while the inspector edits its active-orientation rectangle
  and refreshes both previews immediately.
- Each preview also supports direct manipulation: click an element to select
  it, drag it to move, or drag its cyan corner handle to resize it. Canvas
  bounds are enforced during the gesture; document-level problems remain
  visible and block saving until corrected.
- Undo and redo operate on complete layout edits from either the canvas or
  numeric inspector. Dirty state follows the history cursor, so returning to
  the last saved revision clears the unsaved marker.

Orientation-specific geometry is authored in
`launcher/main/ui/launcher_layout.json` and
`launcher/main/ui/control_center_layout.json`, then deterministically baked
into their matching generated headers. The checked-in headers are the only
form used by firmware; the device does not parse JSON or run a layout solver.

The editor loads each JSON file into its own typed C++ document and validates
canvas bounds, minimum targets and overlap. Save and Ctrl+S update only the
active source document when it is valid. Bake firmware layout then invokes
that document's canonical Python generator as an explicit, separate step, so
preview edits cannot silently change a device build and the editor does not
grow a second implementation of the bake rules.

Firmware navigation is explicit state rather than an editor-only simulation:
an inward swipe from the logical top opens Control Center from Launcher, and
an inward swipe from the logical bottom closes it. Logical edges are mapped to
physical touch edges for all four display rotations. The first Control Center
slice renders connectivity controls, volume and brightness sliders, and two
notification rows; service actions and live connectivity state remain future
integration work.

Dependencies are fetched into the untracked build directory rather than
vendored into firmware source.

## Build

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Debug
cmake --build engine/build --config Debug
ctest --test-dir engine/build --output-on-failure
```

The executable is named `engine`.

## Tests and coverage

CTest is the single test entry point. Host C++ behavior uses GoogleTest and
GoogleMock, firmware C continues to use Unity, and layout generators use
Python's `unittest`. New behavior should begin with a failing focused test,
then the smallest implementation that makes it pass, followed by refactoring
with the suite green.

GoogleTest is pinned to v1.17.0. Coverage uses gcovr 8.6 and is gated at 80%
line coverage and 70% branch coverage for deterministic Engine-owned logic:
the launcher document, generic edit history and host runtime boundary. SDL,
Dear ImGui, generated code and third-party dependencies are excluded from the
numeric gate. The gate now covers both typed documents, generic history, the
runtime boundary and system navigation. Compiler-generated throw and
unreachable branches are excluded as well; rendering and interaction paths
require integration or visual regression tests instead.

```sh
python -m pip install gcovr==8.6
cmake -S engine -B engine/build-coverage \
  -DCMAKE_BUILD_TYPE=Debug -DENGINE_ENABLE_COVERAGE=ON
cmake --build engine/build-coverage --target engine_coverage
```

The last command runs the tests, enforces both thresholds and writes an HTML
report to `engine/build-coverage/coverage/index.html` plus Cobertura XML to
`engine/build-coverage/coverage/coverage.xml`.
