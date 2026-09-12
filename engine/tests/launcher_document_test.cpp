#include "launcher_document.h"

#include <filesystem>
#include <string>
#include <utility>

int
main() {
    std::string error;
    std::optional<LauncherDocument> loaded = LauncherDocument::load(ENGINE_TEST_LAYOUT_PATH, error);
    if (!loaded || !error.empty()) {
        return 1;
    }

    LauncherDocument document = std::move(*loaded);
    LauncherLayout& landscape = document.layout(LauncherOrientation::Landscape);
    if (landscape.canvas_width != 448 || landscape.canvas_height != 368 || !document.validate().empty()) {
        return 1;
    }

    const int original_library_x = landscape.rects[static_cast<std::size_t>(LauncherElement::Library)].x;
    LauncherEditHistory history(document);
    landscape.rects[static_cast<std::size_t>(LauncherElement::Library)].x++;
    document.mark_dirty();
    history.commit(document);
    if (!history.can_undo() || !document.dirty() || !history.undo(document) || document.dirty()
        || document.layout(LauncherOrientation::Landscape).rects[static_cast<std::size_t>(LauncherElement::Library)].x
               != original_library_x
        || !history.can_redo() || !history.redo(document) || !document.dirty()) {
        return 1;
    }
    history.mark_saved(document);
    if (document.dirty() || !history.undo(document) || !document.dirty() || !history.redo(document)
        || document.dirty()) {
        return 1;
    }
    history.undo(document);

    LauncherLayout& restored_landscape = document.layout(LauncherOrientation::Landscape);
    const LauncherRect original = restored_landscape.rects[static_cast<std::size_t>(LauncherElement::Library)];
    restored_landscape.rects[static_cast<std::size_t>(LauncherElement::Library)] = {100, 98, 125, 173};
    if (document.validate().empty()) {
        return 1;
    }
    restored_landscape.rects[static_cast<std::size_t>(LauncherElement::Library)] = original;

    const LauncherRect original_status = restored_landscape.rects[static_cast<std::size_t>(LauncherElement::StatusBar)];
    restored_landscape.rects[static_cast<std::size_t>(LauncherElement::StatusBar)].height = 100;
    if (document.validate().empty()) {
        return 1;
    }
    restored_landscape.rects[static_cast<std::size_t>(LauncherElement::StatusBar)] = original_status;

    const std::filesystem::path roundtrip_path =
        std::filesystem::temp_directory_path() / "engine-launcher-layout-test.json";
    std::filesystem::copy_file(ENGINE_TEST_LAYOUT_PATH, roundtrip_path,
                               std::filesystem::copy_options::overwrite_existing);
    loaded = LauncherDocument::load(roundtrip_path, error);
    if (!loaded || !loaded->save(error)) {
        return 1;
    }
    loaded = LauncherDocument::load(roundtrip_path, error);
    std::error_code remove_error;
    std::filesystem::remove(roundtrip_path, remove_error);
    return loaded && error.empty() && !remove_error ? 0 : 1;
}
