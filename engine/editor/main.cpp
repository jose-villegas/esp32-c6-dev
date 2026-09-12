#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "imgui_internal.h"

#include "engine/preview_surface.h"
#include "engine/runtime.h"

namespace {

struct Preview {
    const char* name;
    int width;
    int height;
    std::vector<uint16_t> pixels;
    SDL_Texture* texture = nullptr;
};

bool
create_preview(SDL_Renderer* renderer, Preview& preview) {
    preview.pixels.resize(static_cast<size_t>(preview.width) * static_cast<size_t>(preview.height));
    engine_preview_surface_t surface = {
        preview.width,
        preview.height,
        preview.pixels.data(),
    };
    if (!engine_preview_render_launcher(&surface)) {
        return false;
    }

    preview.texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, preview.width, preview.height);
    if (!preview.texture) {
        return false;
    }
    SDL_SetTextureScaleMode(preview.texture, SDL_ScaleModeNearest);
    return SDL_UpdateTexture(preview.texture, nullptr, preview.pixels.data(),
                             preview.width * static_cast<int>(sizeof(uint16_t)))
           == 0;
}

void
draw_preview(Preview& preview, float available_width) {
    ImGui::TextUnformatted(preview.name);
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", preview.width, preview.height);

    const float scale = std::min(1.0f, available_width / static_cast<float>(preview.width));
    const ImVec2 size(preview.width * scale, preview.height * scale);
    ImGui::Image(reinterpret_cast<ImTextureID>(preview.texture), size);
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
draw_editor(Preview& landscape, Preview& portrait) {
    bool reset_layout = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::MenuItem("Open layout...", nullptr, false, false);
            ImGui::MenuItem("Save", "Ctrl+S", false, false);
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

    draw_dockspace(reset_layout);

    ImGui::SetNextWindowSize(ImVec2(220, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("Layout data will populate this tree.");
    if (ImGui::TreeNodeEx("Launcher", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BulletText("Status bar");
        ImGui::BulletText("App rail");
        ImGui::BulletText("Page indicator");
        ImGui::TreePop();
    }
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(950, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("Preview");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::BeginChild("Landscape", ImVec2(half, 0), ImGuiChildFlags_Borders);
    draw_preview(landscape, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Portrait", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_preview(portrait, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(300, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector");
    ImGui::TextDisabled("Select a layout element to edit its authored data.");
    ImGui::SeparatorText("Display");
    ImGui::Text("Landscape  448 x 368");
    ImGui::Text("Portrait   368 x 448");
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(950, 160), ImGuiCond_FirstUseEver);
    ImGui::Begin("Problems");
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.82f, 1.0f),
                       "Firmware runtime ready: gfx.c and Microui initialized in-process.");
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.82f, 1.0f),
                       "Current firmware launcher rendered at both device orientations.");
    ImGui::TextDisabled("Layout validation is the next engine module.");
    ImGui::End();
}

} // namespace

int
main() {
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

    Preview landscape = {"Landscape", 448, 368, {}, nullptr};
    Preview portrait = {"Portrait", 368, 448, {}, nullptr};
    if (!create_preview(renderer, landscape) || !create_preview(renderer, portrait)) {
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
        draw_editor(landscape, portrait);
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
