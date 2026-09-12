#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "control_center_document.h"
#include "core/dockspace.h"
#include "core/rgb565_texture.h"
#include "engine/preview_surface.h"
#include "engine/runtime.h"
#include "launcher_document.h"

namespace {

struct Preview {
    const char* name;
    LayoutOrientation orientation;
    Rgb565Texture surface;

    Preview(const char* preview_name, LayoutOrientation preview_orientation, int width, int height)
        : name(preview_name), orientation(preview_orientation), surface(width, height) {}
};

enum class DragMode {
    None,
    Move,
    Resize,
};

struct CanvasInteraction {
    DragMode mode = DragMode::None;
    LayoutOrientation orientation = LayoutOrientation::Landscape;
    std::size_t element_index = 0;
    LayoutRect original = {};
    ImVec2 pointer_origin = {};
};

bool
contains(const LayoutRect& rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

template <typename Layout>
std::optional<std::size_t>
hit_test(const Layout& layout, float x, float y) {
    for (std::size_t index = layout.rects.size(); index > 0; index--) {
        if (contains(layout.rects[index - 1], x, y)) {
            return index - 1;
        }
    }
    return std::nullopt;
}

bool
same_rect(const LayoutRect& first, const LayoutRect& second) {
    return first.x == second.x && first.y == second.y && first.width == second.width && first.height == second.height;
}

engine_launcher_layout_t
runtime_layout(const LauncherLayout& source) {
    engine_launcher_layout_t result = {
        source.canvas_width,
        source.canvas_height,
        {},
    };
    for (std::size_t index = 0; index < source.rects.size(); index++) {
        const LauncherRect& rect = source.rects[index];
        result.rects[index] = {rect.x, rect.y, rect.width, rect.height};
    }
    return result;
}

engine_control_center_layout_t
runtime_layout(const ControlCenterLayout& source) {
    engine_control_center_layout_t result = {source.canvas_width, source.canvas_height, {}};
    for (std::size_t index = 0; index < source.rects.size(); index++) {
        const LayoutRect& rect = source.rects[index];
        result.rects[index] = {rect.x, rect.y, rect.width, rect.height};
    }
    return result;
}

bool
render_preview(Preview& preview, const LauncherLayout& layout) {
    const engine_launcher_layout_t authored = runtime_layout(layout);
    engine_preview_surface_t surface = {
        preview.surface.width(),
        preview.surface.height(),
        preview.surface.pixels(),
    };
    std::string error;
    return engine_preview_render_launcher_layout(&surface, &authored) && preview.surface.upload(error);
}

bool
render_preview(Preview& preview, const ControlCenterLayout& layout) {
    const engine_control_center_layout_t authored = runtime_layout(layout);
    engine_preview_surface_t surface = {preview.surface.width(), preview.surface.height(), preview.surface.pixels()};
    std::string error;
    return engine_preview_render_control_center_layout(&surface, &authored) && preview.surface.upload(error);
}

std::string
shell_argument(const std::filesystem::path& path) {
    const std::string value = path.string();
#ifdef _WIN32
    if (value.find('"') != std::string::npos || value.find('%') != std::string::npos) {
        return {};
    }
    return '"' + value + '"';
#else
    std::string quoted = "'";
    for (char character : value) {
        quoted += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return quoted + "'";
#endif
}

bool
bake_layout(const std::filesystem::path& source, const char* generator_name, const char* output_name, bool check_only,
            std::string& error) {
#ifndef ENGINE_PYTHON_EXECUTABLE
    error = "Python was not available when Engine was configured";
    return false;
#else
    const std::filesystem::path project_root = ENGINE_PROJECT_ROOT;
    const std::filesystem::path generator = project_root / "launcher" / "tools" / generator_name;
    const std::filesystem::path output = project_root / "launcher" / "main" / "ui" / output_name;
    const std::string python_argument = shell_argument(ENGINE_PYTHON_EXECUTABLE);
    const std::string generator_argument = shell_argument(generator);
    const std::string source_argument = shell_argument(source);
    const std::string output_argument = shell_argument(output);
    if (python_argument.empty() || generator_argument.empty() || source_argument.empty() || output_argument.empty()) {
        error = "A bake path contains unsupported shell characters";
        return false;
    }

    // The canonical Python generator remains the only implementation that
    // writes firmware geometry; the editor only launches it on explicit input.
    std::string command = python_argument + " " + generator_argument + " " + source_argument + " " + output_argument;
    if (check_only) {
        command += " --check";
    }
#ifdef _WIN32
    command = '"' + command + '"';
#endif
    if (std::system(command.c_str()) != 0) {
        error = "Layout generator failed";
        return false;
    }
    error.clear();
    return true;
#endif
}

bool
bake_launcher_layout(const LauncherDocument& document, bool check_only, std::string& error) {
    return bake_layout(document.path(), "gen_launcher_layout.py", "launcher_layout_generated.h", check_only, error);
}

bool
bake_control_center_layout(const ControlCenterDocument& document, bool check_only, std::string& error) {
    return bake_layout(document.path(), "gen_control_center_layout.py", "control_center_layout_generated.h", check_only,
                       error);
}

template <typename Layout>
bool
create_preview(SDL_Renderer* renderer, Preview& preview, const Layout& layout) {
    std::string error;
    if (!preview.surface.create(renderer, error)) {
        return false;
    }
    return render_preview(preview, layout);
}

template <typename Layout>
void constrain_rect(LayoutRect& rect, const Layout& layout);

template <typename Layout>
bool
draw_preview(Preview& preview, Layout& layout, std::size_t& selected, LayoutOrientation& active_orientation,
             CanvasInteraction& interaction, float available_width) {
    ImGui::TextUnformatted(preview.name);
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", preview.surface.width(), preview.surface.height());

    const float scale = std::min(1.0f, available_width / static_cast<float>(preview.surface.width()));
    const ImVec2 size(preview.surface.width() * scale, preview.surface.height() * scale);
    const ImVec2 image_position = ImGui::GetCursorScreenPos();
    const std::string canvas_id = std::string("##canvas-") + layout_orientation_id(preview.orientation);
    ImGui::InvisibleButton(canvas_id.c_str(), size, ImGuiButtonFlags_MouseButtonLeft);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(preview.surface.native_handle()), image_position,
                                         ImVec2(image_position.x + size.x, image_position.y + size.y));

    bool changed = false;
    const ImVec2 pointer = ImGui::GetIO().MousePos;
    const float local_x = (pointer.x - image_position.x) / scale;
    const float local_y = (pointer.y - image_position.y) / scale;
    LayoutRect& selected_rect = layout.rects[selected];
    const ImVec2 selected_maximum(image_position.x + (selected_rect.x + selected_rect.width) * scale,
                                  image_position.y + (selected_rect.y + selected_rect.height) * scale);
    const bool over_resize_handle = pointer.x >= selected_maximum.x - 12.0f && pointer.x <= selected_maximum.x
                                    && pointer.y >= selected_maximum.y - 12.0f && pointer.y <= selected_maximum.y;

    if (ImGui::IsItemHovered()) {
        if (over_resize_handle) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        } else if (contains(selected_rect, local_x, local_y)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const std::optional<std::size_t> hit = hit_test(layout, local_x, local_y);
        if (hit) {
            const bool resize = *hit == selected && over_resize_handle;
            selected = *hit;
            active_orientation = preview.orientation;
            interaction.mode = resize ? DragMode::Resize : DragMode::Move;
            interaction.orientation = preview.orientation;
            interaction.element_index = *hit;
            interaction.original = layout.rects[*hit];
            interaction.pointer_origin = pointer;
        }
    }

    if (interaction.mode != DragMode::None && interaction.orientation == preview.orientation) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction.mode = DragMode::None;
        } else {
            LayoutRect next = interaction.original;
            const int delta_x = static_cast<int>(std::lround((pointer.x - interaction.pointer_origin.x) / scale));
            const int delta_y = static_cast<int>(std::lround((pointer.y - interaction.pointer_origin.y) / scale));
            if (interaction.mode == DragMode::Move) {
                next.x += delta_x;
                next.y += delta_y;
            } else {
                next.width += delta_x;
                next.height += delta_y;
            }
            constrain_rect(next, layout);
            LayoutRect& target = layout.rects[interaction.element_index];
            if (!same_rect(target, next)) {
                target = next;
                changed = true;
            }
        }
    }

    const LayoutRect& rect = layout.rects[selected];
    const ImVec2 minimum(image_position.x + rect.x * scale, image_position.y + rect.y * scale);
    const ImVec2 maximum(minimum.x + rect.width * scale, minimum.y + rect.height * scale);
    ImGui::GetWindowDrawList()->AddRect(minimum, maximum, IM_COL32(91, 229, 235, 255), 2.0f, 0, 2.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(maximum.x - 8.0f, maximum.y - 8.0f), maximum,
                                              IM_COL32(91, 229, 235, 255));
    return changed;
}

template <typename Layout>
void
constrain_rect(LayoutRect& rect, const Layout& layout) {
    rect.width = std::clamp(rect.width, 1, layout.canvas_width);
    rect.height = std::clamp(rect.height, 1, layout.canvas_height);
    rect.x = std::clamp(rect.x, 0, layout.canvas_width - rect.width);
    rect.y = std::clamp(rect.y, 0, layout.canvas_height - rect.height);
}

struct RectEditResult {
    bool changed;
    bool committed;
};

template <typename Layout>
RectEditResult
draw_rect_editor(LayoutRect& rect, const Layout& layout) {
    bool changed = false;
    bool committed = false;
    changed |= ImGui::DragInt("X", &rect.x, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragInt("Y", &rect.y, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragInt("Width", &rect.width, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= ImGui::DragInt("Height", &rect.height, 1.0f);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    if (changed) {
        constrain_rect(rect, layout);
    }
    return {changed, committed};
}

enum class SystemDocumentKind {
    Launcher,
    ControlCenter,
};

const char*
launcher_element_id_at(std::size_t index) {
    return launcher_element_id(static_cast<LauncherElement>(index));
}

const char*
launcher_element_label_at(std::size_t index) {
    return launcher_element_label(static_cast<LauncherElement>(index));
}

const char*
control_center_element_id_at(std::size_t index) {
    return control_center_element_id(static_cast<ControlCenterElement>(index));
}

const char*
control_center_element_label_at(std::size_t index) {
    return control_center_element_label(static_cast<ControlCenterElement>(index));
}

template <typename Document, typename History>
bool
draw_editor(Document& document, Preview& landscape, Preview& portrait, std::size_t& selected,
            LayoutOrientation& active_orientation, CanvasInteraction& interaction, History& history,
            const char* screen_name, const char* source_name, const char* baked_name, std::size_t element_count,
            const char* (*element_id)(std::size_t), const char* (*element_label)(std::size_t),
            bool (*bake_document)(const Document&, bool, std::string&), SystemDocumentKind current_screen,
            SystemDocumentKind& active_screen, std::string& notice) {
    bool reset_layout = false;
    bool save_requested = false;
    bool bake_requested = false;
    bool undo_requested = false;
    bool redo_requested = false;
    bool preview_changed = false;
    bool canvas_changed = false;
    const std::vector<std::string> problems_before_edit = document.validate();

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::MenuItem("Open layout...", nullptr, false, false);
            save_requested = ImGui::MenuItem("Save", "Ctrl+S", false, document.dirty());
            ImGui::Separator();
            bake_requested = ImGui::MenuItem("Bake firmware layout", nullptr, false,
                                             !document.dirty() && problems_before_edit.empty());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            undo_requested = ImGui::MenuItem("Undo", "Ctrl+Z", false, history.can_undo());
            redo_requested = ImGui::MenuItem("Redo", "Ctrl+Y", false, history.can_redo());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            reset_layout = ImGui::MenuItem("Reset workspace");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && document.dirty()) {
        save_requested = true;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        redo_requested = io.KeyShift && history.can_redo();
        undo_requested = !io.KeyShift && history.can_undo();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && history.can_redo()) {
        redo_requested = true;
    }

    if (undo_requested || redo_requested) {
        interaction.mode = DragMode::None;
        const bool applied = undo_requested ? history.undo(document) : history.redo(document);
        if (applied) {
            preview_changed = true;
            notice = undo_requested ? "Undo" : "Redo";
        }
    }

    draw_editor_dockspace(reset_layout);

    ImGui::SetNextWindowSize(ImVec2(220, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("System Workspace");
    if (ImGui::Selectable("Launcher", active_screen == SystemDocumentKind::Launcher)) {
        active_screen = SystemDocumentKind::Launcher;
    }
    if (ImGui::Selectable("Control Center", active_screen == SystemDocumentKind::ControlCenter)) {
        active_screen = SystemDocumentKind::ControlCenter;
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", source_name);
    if (ImGui::TreeNodeEx(screen_name, ImGuiTreeNodeFlags_DefaultOpen)) {
        for (std::size_t index = 0; index < element_count; index++) {
            if (ImGui::Selectable(element_label(index), selected == index)) {
                selected = index;
            }
        }
        ImGui::TreePop();
    }
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(950, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("Preview");
    if (current_screen == SystemDocumentKind::Launcher) {
        if (ImGui::Button("Simulate swipe down")) {
            active_screen = SystemDocumentKind::ControlCenter;
        }
    } else if (ImGui::Button("Simulate swipe up")) {
        active_screen = SystemDocumentKind::Launcher;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Click to select, drag to move, or drag the cyan corner to resize.");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    const bool was_canvas_editing = interaction.mode != DragMode::None;
    ImGui::BeginChild("Landscape", ImVec2(half, 0), ImGuiChildFlags_Borders);
    canvas_changed |= draw_preview(landscape, document.layout(LayoutOrientation::Landscape), selected,
                                   active_orientation, interaction, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Portrait", ImVec2(0, 0), ImGuiChildFlags_Borders);
    canvas_changed |= draw_preview(portrait, document.layout(LayoutOrientation::Portrait), selected, active_orientation,
                                   interaction, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::End();

    if (canvas_changed) {
        document.mark_dirty();
        preview_changed = true;
        notice.clear();
    }
    if (was_canvas_editing && interaction.mode == DragMode::None) {
        history.commit(document);
    }

    ImGui::SetNextWindowSize(ImVec2(300, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector");
    ImGui::TextUnformatted(element_label(selected));
    ImGui::TextDisabled("%s", element_id(selected));
    ImGui::SeparatorText("Orientation");
    if (ImGui::RadioButton("Landscape", active_orientation == LayoutOrientation::Landscape)) {
        active_orientation = LayoutOrientation::Landscape;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Portrait", active_orientation == LayoutOrientation::Portrait)) {
        active_orientation = LayoutOrientation::Portrait;
    }

    auto& layout = document.layout(active_orientation);
    LayoutRect& rect = layout.rects[selected];
    ImGui::TextDisabled("Canvas %d x %d", layout.canvas_width, layout.canvas_height);
    ImGui::SeparatorText("Rectangle");
    const RectEditResult rect_edit = draw_rect_editor(rect, layout);
    if (rect_edit.changed) {
        document.mark_dirty();
        preview_changed = true;
    }
    if (rect_edit.committed) {
        history.commit(document);
    }

    const std::vector<std::string> problems = document.validate();
    ImGui::BeginDisabled(!document.dirty() || !problems.empty());
    if (ImGui::Button("Save source")) {
        save_requested = true;
    }
    ImGui::EndDisabled();
    if (document.dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "Unsaved");
    }
    ImGui::BeginDisabled(document.dirty() || !problems.empty());
    if (ImGui::Button("Bake firmware layout")) {
        bake_requested = true;
    }
    ImGui::EndDisabled();
    ImGui::End();

    if (save_requested) {
        std::string error;
        if (document.save(error)) {
            history.mark_saved(document);
            notice = "Saved " + document.path().string();
        } else {
            notice = "Save failed: " + error;
        }
    }
    if (bake_requested) {
        std::string error;
        if (bake_document(document, false, error)) {
            notice = std::string("Baked ") + baked_name;
        } else {
            notice = "Bake failed: " + error;
        }
    }

    ImGui::SetNextWindowSize(ImVec2(950, 160), ImGuiCond_FirstUseEver);
    ImGui::Begin("Problems");
    if (problems.empty()) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.82f, 1.0f), "Layout valid in both orientations.");
    } else {
        for (const std::string& problem : problems) {
            ImGui::BulletText("%s", problem.c_str());
        }
    }
    if (!notice.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", notice.c_str());
    }
    ImGui::TextDisabled("Save updates authored JSON; Bake explicitly regenerates firmware geometry.");
    ImGui::End();

    return preview_changed || active_screen != current_screen;
}

} // namespace

int
main(int argument_count, char** arguments) {
    const std::filesystem::path launcher_path =
        std::filesystem::path(ENGINE_PROJECT_ROOT) / "launcher" / "main" / "ui" / "launcher_layout.json";
    const std::filesystem::path control_center_path =
        std::filesystem::path(ENGINE_PROJECT_ROOT) / "launcher" / "main" / "ui" / "control_center_layout.json";
    std::string document_error;
    std::optional<LauncherDocument> loaded_launcher = LauncherDocument::load(launcher_path, document_error);
    if (!loaded_launcher) {
        std::fprintf(stderr, "Launcher document failed to load: %s\n", document_error.c_str());
        return 1;
    }
    std::optional<ControlCenterDocument> loaded_control_center =
        ControlCenterDocument::load(control_center_path, document_error);
    if (!loaded_control_center) {
        std::fprintf(stderr, "Control Center document failed to load: %s\n", document_error.c_str());
        return 1;
    }
    LauncherDocument launcher_document = std::move(*loaded_launcher);
    ControlCenterDocument control_center_document = std::move(*loaded_control_center);

    if (argument_count == 2 && std::string(arguments[1]) == "--check-bake") {
        std::string bake_error;
        if (!bake_launcher_layout(launcher_document, true, bake_error)
            || !bake_control_center_layout(control_center_document, true, bake_error)) {
            std::fprintf(stderr, "System layout bake check failed: %s\n", bake_error.c_str());
            return 1;
        }
        return 0;
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1440, 900,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer =
        window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (window && !renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!window || !renderer) {
        std::fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        return 1;
    }

    if (!engine_runtime_init()) {
        std::fprintf(stderr, "Firmware runtime initialization failed\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    Preview landscape("Landscape", LayoutOrientation::Landscape, 448, 368);
    Preview portrait("Portrait", LayoutOrientation::Portrait, 368, 448);
    if (!create_preview(renderer, landscape, launcher_document.layout(landscape.orientation))
        || !create_preview(renderer, portrait, launcher_document.layout(portrait.orientation))) {
        std::fprintf(stderr, "Preview texture creation failed: %s\n", SDL_GetError());
        portrait.surface.reset();
        landscape.surface.reset();
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::size_t launcher_selected = static_cast<std::size_t>(LauncherElement::LastPlayed);
    std::size_t control_center_selected = static_cast<std::size_t>(ControlCenterElement::Wifi);
    LayoutOrientation launcher_orientation = LayoutOrientation::Landscape;
    LayoutOrientation control_center_orientation = LayoutOrientation::Landscape;
    CanvasInteraction launcher_interaction;
    CanvasInteraction control_center_interaction;
    LauncherEditHistory launcher_history(launcher_document);
    ControlCenterEditHistory control_center_history(control_center_document);
    SystemDocumentKind active_screen = SystemDocumentKind::Launcher;
    std::string notice;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT
                || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE
                    && event.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        bool preview_changed;
        if (active_screen == SystemDocumentKind::Launcher) {
            preview_changed = draw_editor(
                launcher_document, landscape, portrait, launcher_selected, launcher_orientation, launcher_interaction,
                launcher_history, "Launcher", "launcher_layout.json", "launcher_layout_generated.h",
                static_cast<std::size_t>(LauncherElement::Count), launcher_element_id_at, launcher_element_label_at,
                bake_launcher_layout, SystemDocumentKind::Launcher, active_screen, notice);
        } else {
            preview_changed = draw_editor(
                control_center_document, landscape, portrait, control_center_selected, control_center_orientation,
                control_center_interaction, control_center_history, "Control Center", "control_center_layout.json",
                "control_center_layout_generated.h", static_cast<std::size_t>(ControlCenterElement::Count),
                control_center_element_id_at, control_center_element_label_at, bake_control_center_layout,
                SystemDocumentKind::ControlCenter, active_screen, notice);
        }
        bool rendered = true;
        if (preview_changed) {
            rendered = active_screen == SystemDocumentKind::Launcher
                           ? render_preview(landscape, launcher_document.layout(landscape.orientation))
                                 && render_preview(portrait, launcher_document.layout(portrait.orientation))
                           : render_preview(landscape, control_center_document.layout(landscape.orientation))
                                 && render_preview(portrait, control_center_document.layout(portrait.orientation));
        }
        if (preview_changed && !rendered) {
            notice = std::string("Preview update failed: ") + SDL_GetError();
        }
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 18, 18, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    portrait.surface.reset();
    landscape.surface.reset();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
