#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class LauncherOrientation {
    Portrait,
    Landscape,
};

enum class LauncherElement : std::size_t {
    StatusBar,
    LastPlayed,
    Library,
    RenderLab,
    PageIndicator,
    Count,
};

struct LauncherRect {
    int x;
    int y;
    int width;
    int height;
};

struct LauncherLayout {
    int canvas_width;
    int canvas_height;
    std::array<LauncherRect, static_cast<std::size_t>(LauncherElement::Count)> rects;
};

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

class LauncherEditHistory {
  public:
    explicit LauncherEditHistory(const LauncherDocument& document);

    void commit(LauncherDocument& document);
    bool undo(LauncherDocument& document);
    bool redo(LauncherDocument& document);
    void mark_saved(LauncherDocument& document);

    bool can_undo() const;
    bool can_redo() const;

  private:
    void sync_dirty(LauncherDocument& document) const;

    static constexpr std::size_t no_saved_cursor = static_cast<std::size_t>(-1);
    std::vector<LauncherDocument> states_;
    std::size_t cursor_ = 0;
    std::size_t saved_cursor_ = 0;
};

const char* launcher_element_id(LauncherElement element);
const char* launcher_element_label(LauncherElement element);
const char* launcher_orientation_id(LauncherOrientation orientation);
