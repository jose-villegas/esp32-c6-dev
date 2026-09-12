#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/edit_history.h"
#include "core/layout.h"

enum class ControlCenterElement : std::size_t {
    Grabber,
    Header,
    Wifi,
    Bluetooth,
    Link,
    Volume,
    Brightness,
    NotificationsHeader,
    LibraryNotification,
    ControllerNotification,
    Count,
};

using ControlCenterLayout = FixedLayout<static_cast<std::size_t>(ControlCenterElement::Count)>;

class ControlCenterDocument {
  public:
    static std::optional<ControlCenterDocument> load(const std::filesystem::path& path, std::string& error);

    bool save(std::string& error);
    std::vector<std::string> validate() const;

    ControlCenterLayout& layout(LayoutOrientation orientation);
    const ControlCenterLayout& layout(LayoutOrientation orientation) const;
    const std::filesystem::path& path() const;

    bool dirty() const;
    void mark_dirty();
    void set_dirty(bool dirty);

  private:
    std::filesystem::path path_;
    ControlCenterLayout portrait_;
    ControlCenterLayout landscape_;
    bool dirty_ = false;
};

struct ControlCenterDocumentGeometryEqual {
    bool operator()(const ControlCenterDocument& first, const ControlCenterDocument& second) const;
};

using ControlCenterEditHistory = EditHistory<ControlCenterDocument, ControlCenterDocumentGeometryEqual>;

const char* control_center_element_id(ControlCenterElement element);
const char* control_center_element_label(ControlCenterElement element);
