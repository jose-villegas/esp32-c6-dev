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

#include "core/dockspace.h"
#include "core/rgb565_texture.h"
#include "engine/preview_surface.h"
#include "engine/runtime.h"
#include "launcher_document.h"

namespace {

struct Preview {
    const char* name;
    LauncherOrientation orientation;
    Rgb565Texture surface;

    Preview(const char* preview_name, LauncherOrientation preview_orientation, int width, int height)
        : name(preview_name), orientation(preview_orientation), surface(width, height) {}
};

enum class DragMode {
    None,
    Move,
    Resize,
};

struct CanvasInteraction {
    DragMode mode = DragMode::None;
    LauncherOrientation orientation = LauncherOrientation::Landscape;
    LauncherElement element = LauncherElement::LastPlayed;
    LauncherRect original = {};
    ImVec2 pointer_origin = {};
};

std::size_t
index_of(LauncherElement element) {
    return static_cast<std::size_t>(element);
}

bool
contains(const LauncherRect& rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

std::optional<LauncherElement>
hit_test(const LauncherLayout& layout, float x, float y) {
    for (std::size_t index = static_cast<std::size_t>(LauncherElement::Count); index > 0; index--) {
        const LauncherElement element = static_cast<LauncherElement>(index - 1);
        if (contains(layout.rects[index_of(element)], x, y)) {
            return element;
        }
    }
    return std::nullopt;
}

bool
same_rect(const LauncherRect& first, const LauncherRect& second) {
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
bake_launcher_layout(const LauncherDocument& document, bool check_only, std::string& error) {
#ifndef ENGINE_PYTHON_EXECUTABLE
    error = "Python was not available when Engine was configured";
    return false;
#else
    const std::filesystem::path project_root = ENGINE_PROJECT_ROOT;
    const std::filesystem::path generator = project_root / "launcher" / "tools" / "gen_launcher_layout.py";
    const std::filesystem::path output = project_root / "launcher" / "main" / "ui" / "launcher_layout_generated.h";
    const std::string python_argument = shell_argument(ENGINE_PYTHON_EXECUTABLE);
    const std::string generator_argument = shell_argument(generator);
    const std::string source_argument = shell_argument(document.path());
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
        error = "Launcher layout generator failed";
        return false;
    }
    error.clear();
    return true;
#endif
}

bool
create_preview(SDL_Renderer* renderer, Preview& preview, const LauncherLayout& layout) {
    std::string error;
    if (!preview.surface.create(renderer, error)) {
        return false;
    }
    return render_preview(preview, layout);
}

void constrain_rect(LauncherRect& rect, const LauncherLayout& layout);

bool
draw_preview(Preview& preview, LauncherLayout& layout, LauncherElement& selected,
             LauncherOrientation& active_orientation, CanvasInteraction& interaction, float available_width) {
    ImGui::TextUnformatted(preview.name);
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", preview.surface.width(), preview.surface.height());

    const float scale = std::min(1.0f, available_width / static_cast<float>(preview.surface.width()));
    const ImVec2 size(preview.surface.width() * scale, preview.surface.height() * scale);
    const ImVec2 image_position = ImGui::GetCursorScreenPos();
    const std::string canvas_id = std::string("##canvas-") + launcher_orientation_id(preview.orientation);
    ImGui::InvisibleButton(canvas_id.c_str(), size, ImGuiButtonFlags_MouseButtonLeft);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(preview.surface.native_handle()), image_position,
                                         ImVec2(image_position.x + size.x, image_position.y + size.y));

    bool changed = false;
    const ImVec2 pointer = ImGui::GetIO().MousePos;
    const float local_x = (pointer.x - image_position.x) / scale;
    const float local_y = (pointer.y - image_position.y) / scale;
    LauncherRect& selected_rect = layout.rects[index_of(selected)];
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
        const std::optional<LauncherElement> hit = hit_test(layout, local_x, local_y);
        if (hit) {
            const bool resize = *hit == selected && over_resize_handle;
            selected = *hit;
            active_orientation = preview.orientation;
            interaction.mode = resize ? DragMode::Resize : DragMode::Move;
            interaction.orientation = preview.orientation;
            interaction.element = *hit;
            interaction.original = layout.rects[index_of(*hit)];
            interaction.pointer_origin = pointer;
        }
    }

    if (interaction.mode != DragMode::None && interaction.orientation == preview.orientation) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction.mode = DragMode::None;
        } else {
            LauncherRect next = interaction.original;
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
            LauncherRect& target = layout.rects[index_of(interaction.element)];
            if (!same_rect(target, next)) {
                target = next;
                changed = true;
            }
        }
    }

    const LauncherRect& rect = layout.rects[index_of(selected)];
    const ImVec2 minimum(image_position.x + rect.x * scale, image_position.y + rect.y * scale);
    const ImVec2 maximum(minimum.x + rect.width * scale, minimum.y + rect.height * scale);
    ImGui::GetWindowDrawList()->AddRect(minimum, maximum, IM_COL32(91, 229, 235, 255), 2.0f, 0, 2.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(maximum.x - 8.0f, maximum.y - 8.0f), maximum,
                                              IM_COL32(91, 229, 235, 255));
    return changed;
}

void
constrain_rect(LauncherRect& rect, const LauncherLayout& layout) {
    rect.width = std::clamp(rect.width, 1, layout.canvas_width);
    rect.height = std::clamp(rect.height, 1, layout.canvas_height);
    rect.x = std::clamp(rect.x, 0, layout.canvas_width - rect.width);
    rect.y = std::clamp(rect.y, 0, layout.canvas_height - rect.height);
}

struct RectEditResult {
    bool changed;
    bool committed;
};

RectEditResult
draw_rect_editor(LauncherRect& rect, const LauncherLayout& layout) {
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

bool
draw_editor(LauncherDocument& document, Preview& landscape, Preview& portrait, LauncherElement& selected,
            LauncherOrientation& active_orientation, CanvasInteraction& interaction, LauncherEditHistory& history,
            std::string& notice) {
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
    ImGui::TextDisabled("launcher_layout.json");
    if (ImGui::TreeNodeEx("Launcher", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (std::size_t index = 0; index < static_cast<std::size_t>(LauncherElement::Count); index++) {
            const LauncherElement element = static_cast<LauncherElement>(index);
            if (ImGui::Selectable(launcher_element_label(element), selected == element)) {
                selected = element;
            }
        }
        ImGui::TreePop();
    }
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(950, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("Preview");
    ImGui::TextDisabled("Click to select, drag to move, or drag the cyan corner to resize.");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    const bool was_canvas_editing = interaction.mode != DragMode::None;
    ImGui::BeginChild("Landscape", ImVec2(half, 0), ImGuiChildFlags_Borders);
    canvas_changed |= draw_preview(landscape, document.layout(LauncherOrientation::Landscape), selected,
                                   active_orientation, interaction, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Portrait", ImVec2(0, 0), ImGuiChildFlags_Borders);
    canvas_changed |= draw_preview(portrait, document.layout(LauncherOrientation::Portrait), selected,
                                   active_orientation, interaction, ImGui::GetContentRegionAvail().x);
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
    ImGui::TextUnformatted(launcher_element_label(selected));
    ImGui::TextDisabled("%s", launcher_element_id(selected));
    ImGui::SeparatorText("Orientation");
    if (ImGui::RadioButton("Landscape", active_orientation == LauncherOrientation::Landscape)) {
        active_orientation = LauncherOrientation::Landscape;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Portrait", active_orientation == LauncherOrientation::Portrait)) {
        active_orientation = LauncherOrientation::Portrait;
    }

    LauncherLayout& layout = document.layout(active_orientation);
    LauncherRect& rect = layout.rects[index_of(selected)];
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
        if (bake_launcher_layout(document, false, error)) {
            notice = "Baked launcher_layout_generated.h";
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

    return preview_changed;
}

} // namespace

int
main(int argument_count, char** arguments) {
    const std::filesystem::path layout_path =
        std::filesystem::path(ENGINE_PROJECT_ROOT) / "launcher" / "main" / "ui" / "launcher_layout.json";
    std::string document_error;
    std::optional<LauncherDocument> loaded = LauncherDocument::load(layout_path, document_error);
    if (!loaded) {
        std::fprintf(stderr, "Launcher document failed to load: %s\n", document_error.c_str());
        return 1;
    }
    LauncherDocument document = std::move(*loaded);

    if (argument_count == 2 && std::string(arguments[1]) == "--check-bake") {
        std::string bake_error;
        if (!bake_launcher_layout(document, true, bake_error)) {
            std::fprintf(stderr, "Launcher layout bake check failed: %s\n", bake_error.c_str());
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

    Preview landscape("Landscape", LauncherOrientation::Landscape, 448, 368);
    Preview portrait("Portrait", LauncherOrientation::Portrait, 368, 448);
    if (!create_preview(renderer, landscape, document.layout(landscape.orientation))
        || !create_preview(renderer, portrait, document.layout(portrait.orientation))) {
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

    LauncherElement selected = LauncherElement::LastPlayed;
    LauncherOrientation active_orientation = LauncherOrientation::Landscape;
    CanvasInteraction interaction;
    LauncherEditHistory history(document);
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
        const bool preview_changed =
            draw_editor(document, landscape, portrait, selected, active_orientation, interaction, history, notice);
        if (preview_changed
            && (!render_preview(landscape, document.layout(landscape.orientation))
                || !render_preview(portrait, document.layout(portrait.orientation)))) {
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
