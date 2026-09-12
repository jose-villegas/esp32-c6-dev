# Plan: the Engine system workspace and its UI layout format

**Status**: in progress. The launcher document, generator, native editor,
real-renderer preview, direct manipulation, undo/redo and explicit bake path
are built on `feat/engine-bootstrap`.

The current objective is the **System Workspace**: a host tool where
firmware-owned screens are authored visually, rendered by the real device
code, validated and baked for firmware. The launcher is its first document;
Control Center, notifications and Settings are the next system-owned screen
family. This is a component of the engine direction in
[`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md).

## Product boundary

Autana is a platform stack, not one undifferentiated engine:

- **System software** owns boot, launcher, Control Center, notifications,
  Settings, app lifecycle and privileged device services.
- **Runtime** is the device-side rendering, input, audio, storage and app
  execution contract.
- **SDK** will package the public headers, manifests, asset tools, build,
  deployment and debugging flow used by external games.
- **Engine** is the host authoring application. Its current module is the
  System Workspace; a separate Game Workspace may arrive when external game
  projects have a concrete format and runtime contract.

The two workspaces may share docking, inspectors, previews, history and
validation infrastructure, but they do not share ownership or document
models. System documents write this firmware repository and may use
privileged services. External games live in their own projects and expose
only shell metadata such as title, icon and launch manifest. An app's own UI
does not become a System Workspace document merely because the launcher can
start it.

### Distribution and module boundary

Engine should eventually be packaged by audience rather than exposing every
workspace to everyone:

- The **internal build** contains the System Workspace and App Workspace.
- The **external developer build** defaults to, or contains only, the App
  Workspace.
- The **runtime package** contains device libraries and APIs without editor
  code.
- The **SDK package** contains public headers, project templates, asset tools,
  packaging, deployment and documentation.

This must become a compile/package boundary, not a hidden menu item. External
developers should not need the firmware repository or receive privileged
system-screen adapters.

The modular split follows the established console-tooling shape: system
software and shell, platform runtime, developer SDK, engine/editor, then
individual title projects. Autana's custom work is the constrained ESP32
runtime, baked UI and real-renderer preview. Engine should reuse SDL2, Dear
ImGui, CMake, JSON and compiler tooling rather than rebuild generic editor
infrastructure.

The current launcher-specific implementation is a vertical slice, not the
final module boundary. Before the System Workspace grows substantially,
extract a small editor core for docking, history, inspectors and preview
infrastructure. System Workspace adapters remain internal; a future App
Workspace can then be the only module shipped to external developers.

The first extraction now covers generic history, dockspace construction and
RGB565 preview texture ownership under `engine/editor/core/`. Inspector
semantics remain launcher-owned until a second system document demonstrates
which parts are genuinely common.

## Test-driven development policy

Engine behavior is developed red-green-refactor from this first vertical
slice. Tests live at the narrowest useful boundary:

- GoogleTest and GoogleMock cover host C++ documents and reusable editor-core
  behavior.
- Unity remains the device/host framework for firmware C.
- Python `unittest` covers deterministic generators.
- CTest is the umbrella invoked locally and by CI.

Coverage is measured with gcovr on a separately instrumented GCC/Clang build.
The initial gate is 80% lines and 70% branches across deterministic,
Engine-owned document, history and runtime-boundary logic. SDL/ImGui window
glue, generated output and third-party sources are outside that number; they
need smoke, interaction or visual regression coverage instead of misleading
unit-test percentages. Compiler-generated throw and unreachable branches are
also excluded. The gate should expand file-by-file as new testable core
modules land and must not be lowered to make a change pass.

---

## This pattern already runs in this repo

`tools/boot_anim_editor_server.py` serves `tools/boot_anim_editor.html` and
answers `POST /render` with **a real frame rendered by the real firmware
code** - `main/gfx/gfx.c` plus `main/boot/boot_anim.c`, unmodified, compiled
for the host. It writes the edited payload to a *scratch* copy of
`boot_anim_timeline.json` placed on the include path ahead of the committed
one, so a draft never touches the real header. It hashes the payload so
scrubbing time never recompiles, and it has a build-and-flash path out the
back.

That is the architecture, running, for one payload. A level editor
(`ems.9`) proposes it for a second. This plan is the third:

| | authored data | generator | rendered by |
|---|---|---|---|
| boot animation | `boot_anim_timeline.json` | `gen_boot_anim_timeline.py` | real `boot_anim.c` + `gfx.c` on host |
| level editor (`ems.9`) | material blocks | bake to a header | real sand code on host |
| **UI editor (this)** | **a screen's layout** | **bake to a header** | **real `gfx.c` + pure geometry on host** |

They share one authoring pattern, even when their documents belong to
different workspaces. The pattern is already proven.

## The foundation now in place

The launcher layout is authored JSON, baked by a generator into a header that
the same pure renderer consumes. The JSON remains human-readable source; the
generated header remains output. The generator validates before emitting and
the shipped artifact is tested independently.

**The device never sees the editor, and never sees JSON.** It links a static
baked table: no runtime layout engine, no solver, no allocation, no RAM
cost - the same deal fonts, icons and the boot timeline already have. That
distinction is the whole reason this is affordable here and LVGL was not:
the cost of a retained UI system is paid at build time, on a laptop, or it
is not paid at all.

## What already exists

- **The document.** `launcher_layout.json` carries stable element IDs and
  geometry for both device orientations.
- **The render path.** `engine_runtime` links the real launcher, Microui and
  `gfx.c` in-process; edits preview without generating or recompiling.
- **The editor shell.** SDL2 and Dear ImGui provide the system hierarchy,
  dual previews, inspector, history, validation and explicit save/bake flow.
- **Independent checks.** The document, runtime bridge, generator and
  checked-in generated header are covered by host tests.

That last one matters more than it looks - see below.

## The data model: grow it from need, and do not build a solver

The trap is a general constraint system. That is the LVGL mistake relocated
to build time: it makes the model unbounded and turns the editor into a
programming language with a mouse.

What the screens in this tree actually need today is roughly six concepts: a
vertical stack, fixed-versus-fill heights, gaps, margins, centred slack, and
rows within a panel. Express those, and add a seventh when a real screen
demands it rather than in anticipation.

**Text is a first-class constrained thing, not a string dropped in a rect.**
Both real defects the brush screen shipped were text-versus-box - a value
box sized by eye at 64px for a string needing 80, and a caption row 232px
wide for wording needing 240 - and the weakness the host preview then found
in portrait is a *scale policy* failing: a long material name steps down
until it is the same size as the caption above it, and the type hierarchy
collapses. So an entry carries its string source, its scale policy and its
box **together**. An editor that cannot say "this will not fit at this
scale" would let you draw those same bugs, visually, and call it a design.

Those defects inform future shared validation; they do not make Sand or any
other external app a System Workspace document.

## The editor is an engine module, and renders in-process

Two decisions, and the first one forces the second.

**Native, cross-platform, not a web page.** The tools in this tree are POSIX
sh so they run "under Git Bash or MSYS on Windows, and natively on Linux and
macOS" (`tools/screenshot.sh`'s own header). The editor holds to the same
bar: one source tree, three platforms, nothing Windows-specific. Web stays a
*target* - something the engine may one day be built for - never the way the
editor draws itself.

**It links the firmware's host-portable C in-process**, rather than spawning
a renderer and reading back an image. `boot_anim_editor_server.py` spawns
and recompiles because its payload is baked into a header the C reads; there
is no way around it there. Layout-as-data removes that tax entirely: nothing
is generated to preview a change, so the editor can mutate a struct, call
the same layout and draw code the firmware calls, and re-render at frame
rate. Direct manipulation needs that - dragging a panel through a subprocess
round trip per frame is not the same product.

That choice also serves the web goal instead of fighting it: the same
host-portable C compiles under Emscripten, so a browser preview later is the
same code, not a second implementation of it.

### The shell: SDL2 + Dear ImGui

The editor starts at `engine/`; UI authoring is its first module rather than
its permanent product boundary. **SDL2 + Dear ImGui** is the host shell.
SDL supplies the cross-platform window, input and framebuffer texture; Dear
ImGui supplies the hierarchy, inspector, docking, menus and text editing an
authoring tool needs. It links the real host-portable C renderer through a C
boundary. The C++ toolchain is isolated to `engine/` and never enters an
ESP-IDF component graph.

**raylib + raygui** remains the pure-C fallback. It would make the first
preview inexpensive, but a serious hierarchy, inspector, asset browser and
timeline would grow editor chrome that Dear ImGui already provides.

**SDL2 + microui** remains useful as a runtime-input simulator, not as the
authoring shell. Dogfooding does not repay implementing docking, file dialogs
and robust editor text input inside the device toolkit.

**Build with CMake.** ESP-IDF already uses it, so it is not a new tool for
anyone on any platform, and both candidate shells ship support for it. The
editor lives under `engine/`, is never part of the firmware build, and like
every other host tool here is absent from `idf.py` and from
`test/run_tests.sh`.

## Phases

1. **Layout as authored data.** The launcher's JSON, generator and baked
   header feed the same renderer used on device. **Built.**

   **Acceptance:** baked rects reproduce the known-good launcher geometry and
   the conversion changes no pixels.

2. **Validation moves into the generator.** It refuses, at bake time and for
   every orientation, geometry that overlaps, leaves the canvas or drops an
   app target below 44px. The C++ document performs the same checks and the
   host suite remains an independent witness. **Built for launcher geometry;
   text-fit validation remains future work.**

3. **The editor shell.** A native window that links the layout and draw code
   directly and renders both orientations side by side - that is where
   composition decisions actually get made. No server, no subprocess, no
   recompile in the preview loop. Loading, saving and explicitly invoking the
   canonical bake step are its file operations. **Built.**

4. **Direct manipulation.** Drag and resize in the editor, writing back to
   the JSON. A format that only a GUI can produce is still unacceptable, so
   authored JSON remains readable and reviewable. **Built for the launcher,
   including undo/redo.**

5. **System Workspace navigation.** Add Control Center as the second
   firmware-owned screen and model the swipe-down transition from Launcher.
   The hierarchy becomes a system screen/state navigator, while each screen
   keeps its own typed document adapter, renderer, validation and bake path.

6. **External game workflow, later.** Define a separate Game Workspace only
   after the runtime API, package format and app manifest are concrete. Do
   not use an existing app such as Sand merely to make the System Workspace
   appear generic.

## Considered and rejected

- **A runtime layout engine.** The thing microui was chosen over. Layout
  resolved on device costs RAM and cycles for a picture that is identical
  every frame; bake it instead.
- **A general constraint solver.** Unbounded model, unbounded editor, and
  every screen in this tree is a stack of panels. Revisit only when a real
  screen cannot be expressed.
- **Round-tripping generated C.** The generated header is output and never
  input - the convention every other generated file here already follows.
  Parsing back what a generator emitted is how the authored source and the
  artifact drift.
- **A JavaScript reimplementation of the renderer for the editor.** The boot
  animation editor rejected exactly this and says so in its own header: it
  renders through the real C rather than a JS twin. A preview that is not
  the shipping renderer is a preview of something that does not exist.

- **A browser-hosted editor at all**, which is what the boot animation
  editor is. It suits a timeline with a scrubber; a layout editor wants
  direct manipulation, and that wants in-process rendering. Accepted
  consequence: two editor architectures coexist until the older one is
  either migrated or retired. Not a reason to make this one a page.

- **Anything platform-specific** - Win32, WinUI, Cocoa, GTK-only. One source
  tree has to serve Windows, Linux and macOS, which is the same bar every
  shell script here already meets.
- **Editing the mockup instead.** A design image is an input to authoring,
  not the authored artifact. The brush screen already diverges from its
  mockup in two accepted places, and those decisions live in the plan, not
  in a PNG.

## Related

- [`../Building-a-Screen.md`](../Building-a-Screen.md) - how a screen is built by hand today
- [`../UI-Lessons.md`](../UI-Lessons.md) - what the constraints above cost to learn
- [`Icon-Baker-Plan.md`](Icon-Baker-Plan.md) - the same authored-data-to-baked-header pattern, for artwork
- [`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md) - the engine direction this serves
