#include "core/dockspace.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace {

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

} // namespace

void
draw_editor_dockspace(bool reset_layout) {
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
        // DockBuilder seeds the layout while preserving later user changes.
        build_default_layout(dockspace_id, viewport->WorkSize);
    }
    ImGui::DockSpace(dockspace_id);
    ImGui::End();
}
