#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/edit_history.h"
#include "core/layout.h"

using LauncherOrientation = LayoutOrientation;

enum class LauncherElement : std::size_t {
    StatusBar,
    LastPlayed,
    Library,
    RenderLab,
    PageIndicator,
    Count,
};

using LauncherRect = LayoutRect;
using LauncherLayout = FixedLayout<static_cast<std::size_t>(LauncherElement::Count)>;

class LauncherDocument {
  public:
    static std::optional<LauncherDocument> load(const std::filesystem::path& path, std::string& error);

    bool save(std::string& error);
    std::vector<std::string> validate() const;

    LauncherLayout& layout(LauncherOrientation orientation);
    const LauncherLayout& layout(LauncherOrientation orientation) const;
    const std::filesystem::path& path() const;

    bool dirty() const;
    void mark_dirty();
    void set_dirty(bool dirty);

  private:
    std::filesystem::path path_;
    LauncherLayout portrait_;
    LauncherLayout landscape_;
    bool dirty_ = false;
};

struct LauncherDocumentGeometryEqual {
    bool operator()(const LauncherDocument& first, const LauncherDocument& second) const;
};

using LauncherEditHistory = EditHistory<LauncherDocument, LauncherDocumentGeometryEqual>;

const char* launcher_element_id(LauncherElement element);
const char* launcher_element_label(LauncherElement element);
const char* launcher_orientation_id(LauncherOrientation orientation);
