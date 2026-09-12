#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "imgui_internal.h"

#include "engine/preview_surface.h"
#include "engine/runtime.h"
#include "launcher_document.h"

namespace {

struct Preview {
    const char* name;
    LauncherOrientation orientation;
    int width;
    int height;
    std::vector<uint16_t> pixels;
    SDL_Texture* texture = nullptr;
};

std::size_t
index_of(LauncherElement element) {
    return static_cast<std::size_t>(element);
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
        preview.width,
        preview.height,
        preview.pixels.data(),
    };
    return engine_preview_render_launcher_layout(&surface, &authored)
           && SDL_UpdateTexture(preview.texture, nullptr, preview.pixels.data(),
                                preview.width * static_cast<int>(sizeof(uint16_t)))
                  == 0;
}

bool
create_preview(SDL_Renderer* renderer, Preview& preview, const LauncherLayout& layout) {
    preview.pixels.resize(static_cast<std::size_t>(preview.width) * static_cast<std::size_t>(preview.height));
    preview.texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, preview.width, preview.height);
    if (!preview.texture) {
        return false;
    }
    SDL_SetTextureScaleMode(preview.texture, SDL_ScaleModeNearest);
    return render_preview(preview, layout);
}

void
draw_preview(Preview& preview, const LauncherLayout& layout, LauncherElement selected, float available_width) {
    ImGui::TextUnformatted(preview.name);
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", preview.width, preview.height);

    const float scale = std::min(1.0f, available_width / static_cast<float>(preview.width));
    const ImVec2 size(preview.width * scale, preview.height * scale);
    const ImVec2 image_position = ImGui::GetCursorScreenPos();
    ImGui::Image(reinterpret_cast<ImTextureID>(preview.texture), size);

    const LauncherRect& rect = layout.rects[index_of(selected)];
    const ImVec2 minimum(image_position.x + rect.x * scale, image_position.y + rect.y * scale);
    const ImVec2 maximum(minimum.x + rect.width * scale, minimum.y + rect.height * scale);
    ImGui::GetWindowDrawList()->AddRect(minimum, maximum, IM_COL32(91, 229, 235, 255), 2.0f, 0, 2.0f);
}

void
build_default_layout(ImGuiID dockspace_id, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    ImGuiID center_id = dockspace_id;
    ImGuiID hierarchy_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.18f, nullptr, &center_id);
    ImGuiID inspector_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.24f, nullptr, &center_id);
    ImGuiID problems_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down, 0.24f, nullptr, &center_id);

    ImGui::DockBuilderDockWindow("Hierarchy", hierarchy_id);
    ImGui::DockBuilderDockWindow("Preview", center_id);
    ImGui::DockBuilderDockWindow("Inspector", inspector_id);
    ImGui::DockBuilderDockWindow("Problems", problems_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

void
draw_dockspace(bool reset_layout) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
                                   | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                   | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Engine workspace", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspace_id = ImGui::GetID("Engine dockspace");
    if (reset_layout || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        // DockBuilder is internal, but it is the only API that can seed a layout
        // while still allowing ImGui to persist the user's later adjustments.
        build_default_layout(dockspace_id, viewport->WorkSize);
    }
    ImGui::DockSpace(dockspace_id);
    ImGui::End();
}

void
constrain_rect(LauncherRect& rect, const LauncherLayout& layout) {
    rect.width = std::clamp(rect.width, 1, layout.canvas_width);
    rect.height = std::clamp(rect.height, 1, layout.canvas_height);
    rect.x = std::clamp(rect.x, 0, layout.canvas_width - rect.width);
    rect.y = std::clamp(rect.y, 0, layout.canvas_height - rect.height);
}

bool
draw_rect_editor(LauncherRect& rect, const LauncherLayout& layout) {
    bool changed = false;
    changed |= ImGui::DragInt("X", &rect.x, 1.0f);
    changed |= ImGui::DragInt("Y", &rect.y, 1.0f);
    changed |= ImGui::DragInt("Width", &rect.width, 1.0f);
    changed |= ImGui::DragInt("Height", &rect.height, 1.0f);
    if (changed) {
        constrain_rect(rect, layout);
    }
    return changed;
}

bool
draw_editor(LauncherDocument& document, Preview& landscape, Preview& portrait, LauncherElement& selected,
            LauncherOrientation& active_orientation, std::string& notice) {
    bool reset_layout = false;
    bool save_requested = false;
    bool preview_changed = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::MenuItem("Open layout...", nullptr, false, false);
            save_requested = ImGui::MenuItem("Save", "Ctrl+S", false, document.dirty());
            ImGui::Separator();
            ImGui::MenuItem("Build firmware...", nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            reset_layout = ImGui::MenuItem("Reset workspace");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && document.dirty()) {
        save_requested = true;
    }

    draw_dockspace(reset_layout);

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
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::BeginChild("Landscape", ImVec2(half, 0), ImGuiChildFlags_Borders);
    draw_preview(landscape, document.layout(LauncherOrientation::Landscape), selected,
                 ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Portrait", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_preview(portrait, document.layout(LauncherOrientation::Portrait), selected, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::End();

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
    if (draw_rect_editor(rect, layout)) {
        document.mark_dirty();
        preview_changed = true;
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
    ImGui::End();

    if (save_requested) {
        std::string error;
        if (document.save(error)) {
            notice = "Saved " + document.path().string();
        } else {
            notice = "Save failed: " + error;
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
    ImGui::TextDisabled("Saving updates authored JSON; checked-in firmware geometry is rebaked separately.");
    ImGui::End();

    return preview_changed;
}

} // namespace

int
main() {
    const std::filesystem::path layout_path =
        std::filesystem::path(ENGINE_PROJECT_ROOT) / "launcher" / "main" / "ui" / "launcher_layout.json";
    std::string document_error;
    std::optional<LauncherDocument> loaded = LauncherDocument::load(layout_path, document_error);
    if (!loaded) {
        std::fprintf(stderr, "Launcher document failed to load: %s\n", document_error.c_str());
        return 1;
    }
    LauncherDocument document = std::move(*loaded);

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

    Preview landscape = {"Landscape", LauncherOrientation::Landscape, 448, 368, {}, nullptr};
    Preview portrait = {"Portrait", LauncherOrientation::Portrait, 368, 448, {}, nullptr};
    if (!create_preview(renderer, landscape, document.layout(landscape.orientation))
        || !create_preview(renderer, portrait, document.layout(portrait.orientation))) {
        std::fprintf(stderr, "Preview texture creation failed: %s\n", SDL_GetError());
        if (portrait.texture) {
            SDL_DestroyTexture(portrait.texture);
        }
        if (landscape.texture) {
            SDL_DestroyTexture(landscape.texture);
        }
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
        const bool preview_changed = draw_editor(document, landscape, portrait, selected, active_orientation, notice);
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

    SDL_DestroyTexture(portrait.texture);
    SDL_DestroyTexture(landscape.texture);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
