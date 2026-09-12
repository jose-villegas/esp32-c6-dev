#include "control_center_document.h"

#include <array>
#include <fstream>
#include <initializer_list>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::ordered_json;

constexpr std::array<ControlCenterElement, static_cast<std::size_t>(ControlCenterElement::Count)> elements = {
    ControlCenterElement::Grabber,
    ControlCenterElement::Header,
    ControlCenterElement::Wifi,
    ControlCenterElement::Bluetooth,
    ControlCenterElement::Link,
    ControlCenterElement::Volume,
    ControlCenterElement::Brightness,
    ControlCenterElement::NotificationsHeader,
    ControlCenterElement::LibraryNotification,
    ControlCenterElement::ControllerNotification,
};

constexpr std::array<ControlCenterElement, 7> interactive_elements = {
    ControlCenterElement::Wifi,
    ControlCenterElement::Bluetooth,
    ControlCenterElement::Link,
    ControlCenterElement::Volume,
    ControlCenterElement::Brightness,
    ControlCenterElement::LibraryNotification,
    ControlCenterElement::ControllerNotification,
};

std::size_t
index_of(ControlCenterElement element) {
    return static_cast<std::size_t>(element);
}

void
require_exact_keys(const Json& value, std::initializer_list<const char*> keys, const std::string& context) {
    if (!value.is_object() || value.size() != keys.size()) {
        throw std::runtime_error(context + " has unexpected fields");
    }
    for (const char* key : keys) {
        if (!value.contains(key)) {
            throw std::runtime_error(context + " is missing " + key);
        }
    }
}

LayoutRect
read_rect(const Json& value) {
    if (!value.is_array() || value.size() != 4) {
        throw std::runtime_error("rectangle must be [x, y, width, height]");
    }
    for (const Json& component : value) {
        if (!component.is_number_integer()) {
            throw std::runtime_error("rectangle components must be integers");
        }
    }
    return {value[0].get<int>(), value[1].get<int>(), value[2].get<int>(), value[3].get<int>()};
}

ControlCenterLayout
read_layout(const Json& value) {
    require_exact_keys(value, {"canvas", "rects"}, "orientation");
    const Json& canvas = value.at("canvas");
    if (!canvas.is_array() || canvas.size() != 2 || !canvas[0].is_number_integer() || !canvas[1].is_number_integer()) {
        throw std::runtime_error("canvas must be [width, height]");
    }

    ControlCenterLayout layout = {canvas[0].get<int>(), canvas[1].get<int>(), {}};
    const Json& rects = value.at("rects");
    if (!rects.is_object() || rects.size() != elements.size()) {
        throw std::runtime_error("rects must contain exactly the stable Control Center IDs");
    }
    for (ControlCenterElement element : elements) {
        if (!rects.contains(control_center_element_id(element))) {
            throw std::runtime_error(std::string("rects is missing ") + control_center_element_id(element));
        }
        layout.rects[index_of(element)] = read_rect(rects.at(control_center_element_id(element)));
    }
    return layout;
}

bool
overlaps(const LayoutRect& first, const LayoutRect& second) {
    return first.x < second.x + second.width && second.x < first.x + first.width && first.y < second.y + second.height
           && second.y < first.y + first.height;
}

void
validate_layout(const ControlCenterLayout& layout, LayoutOrientation orientation, std::vector<std::string>& problems) {
    const std::string prefix = std::string(layout_orientation_id(orientation)) + ": ";
    for (ControlCenterElement element : elements) {
        const LayoutRect& rect = layout.rects[index_of(element)];
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 || rect.x + rect.width > layout.canvas_width
            || rect.y + rect.height > layout.canvas_height) {
            problems.push_back(prefix + control_center_element_id(element) + " leaves the canvas");
        }
    }
    for (ControlCenterElement element : interactive_elements) {
        const LayoutRect& rect = layout.rects[index_of(element)];
        if (rect.width < 44 || rect.height < 44) {
            problems.push_back(prefix + control_center_element_id(element) + " is smaller than 44px");
        }
    }
    for (std::size_t first = 0; first < elements.size(); first++) {
        for (std::size_t second = first + 1; second < elements.size(); second++) {
            if (overlaps(layout.rects[index_of(elements[first])], layout.rects[index_of(elements[second])])) {
                problems.push_back(prefix + control_center_element_id(elements[first]) + " overlaps "
                                   + control_center_element_id(elements[second]));
            }
        }
    }
}

bool
same_geometry(const ControlCenterDocument& first, const ControlCenterDocument& second) {
    for (LayoutOrientation orientation : {LayoutOrientation::Landscape, LayoutOrientation::Portrait}) {
        const ControlCenterLayout& first_layout = first.layout(orientation);
        const ControlCenterLayout& second_layout = second.layout(orientation);
        if (first_layout.canvas_width != second_layout.canvas_width
            || first_layout.canvas_height != second_layout.canvas_height) {
            return false;
        }
        for (std::size_t index = 0; index < first_layout.rects.size(); index++) {
            const LayoutRect& a = first_layout.rects[index];
            const LayoutRect& b = second_layout.rects[index];
            if (a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height) {
                return false;
            }
        }
    }
    return true;
}

Json
write_layout(const ControlCenterLayout& layout) {
    Json rects = Json::object();
    for (ControlCenterElement element : elements) {
        const LayoutRect& rect = layout.rects[index_of(element)];
        rects[control_center_element_id(element)] = {rect.x, rect.y, rect.width, rect.height};
    }
    return {{"canvas", {layout.canvas_width, layout.canvas_height}}, {"rects", std::move(rects)}};
}

} // namespace

const char*
control_center_element_id(ControlCenterElement element) {
    static constexpr std::array<const char*, static_cast<std::size_t>(ControlCenterElement::Count)> ids = {
        "grabber",
        "header",
        "wifi",
        "bluetooth",
        "link",
        "volume",
        "brightness",
        "notifications_header",
        "library_notification",
        "controller_notification",
    };
    return ids.at(index_of(element));
}

const char*
control_center_element_label(ControlCenterElement element) {
    static constexpr std::array<const char*, static_cast<std::size_t>(ControlCenterElement::Count)> labels = {
        "Grabber",
        "Header",
        "Wi-Fi",
        "Bluetooth",
        "Link",
        "Volume",
        "Brightness",
        "Notifications header",
        "Library notification",
        "Controller notification",
    };
    return labels.at(index_of(element));
}

std::optional<ControlCenterDocument>
ControlCenterDocument::load(const std::filesystem::path& path, std::string& error) {
    try {
        std::ifstream stream(path);
        if (!stream) {
            throw std::runtime_error("could not open " + path.string());
        }
        Json source;
        stream >> source;
        require_exact_keys(source, {"schema_version", "screen", "element_order", "orientations"}, "document");
        if (source.at("schema_version") != 1 || source.at("screen") != "control_center") {
            throw std::runtime_error("unsupported Control Center document schema");
        }
        Json expected_order = Json::array();
        for (ControlCenterElement element : elements) {
            expected_order.push_back(control_center_element_id(element));
        }
        if (source.at("element_order") != expected_order) {
            throw std::runtime_error("element_order does not match the Control Center schema");
        }
        const Json& orientations = source.at("orientations");
        require_exact_keys(orientations, {"portrait", "landscape"}, "orientations");

        ControlCenterDocument document;
        document.path_ = path;
        document.portrait_ = read_layout(orientations.at("portrait"));
        document.landscape_ = read_layout(orientations.at("landscape"));
        const std::vector<std::string> problems = document.validate();
        if (!problems.empty()) {
            throw std::runtime_error(problems.front());
        }
        error.clear();
        return document;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

bool
ControlCenterDocument::save(std::string& error) {
    const std::vector<std::string> problems = validate();
    if (!problems.empty()) {
        error = problems.front();
        return false;
    }
    Json order = Json::array();
    for (ControlCenterElement element : elements) {
        order.push_back(control_center_element_id(element));
    }
    const Json source = {
        {"schema_version", 1},
        {"screen", "control_center"},
        {"element_order", std::move(order)},
        {"orientations", {{"portrait", write_layout(portrait_)}, {"landscape", write_layout(landscape_)}}},
    };
    std::ofstream stream(path_, std::ios::trunc);
    if (!stream) {
        error = "could not write " + path_.string();
        return false;
    }
    stream << source.dump(2) << '\n';
    if (!stream) {
        error = "failed while writing " + path_.string();
        return false;
    }
    dirty_ = false;
    error.clear();
    return true;
}

std::vector<std::string>
ControlCenterDocument::validate() const {
    std::vector<std::string> problems;
    if (portrait_.canvas_width != 368 || portrait_.canvas_height != 448) {
        problems.emplace_back("portrait: canvas must be 368 x 448");
    }
    if (landscape_.canvas_width != 448 || landscape_.canvas_height != 368) {
        problems.emplace_back("landscape: canvas must be 448 x 368");
    }
    validate_layout(portrait_, LayoutOrientation::Portrait, problems);
    validate_layout(landscape_, LayoutOrientation::Landscape, problems);
    return problems;
}

ControlCenterLayout&
ControlCenterDocument::layout(LayoutOrientation orientation) {
    return orientation == LayoutOrientation::Landscape ? landscape_ : portrait_;
}

const ControlCenterLayout&
ControlCenterDocument::layout(LayoutOrientation orientation) const {
    return orientation == LayoutOrientation::Landscape ? landscape_ : portrait_;
}

const std::filesystem::path&
ControlCenterDocument::path() const {
    return path_;
}

bool
ControlCenterDocument::dirty() const {
    return dirty_;
}

void
ControlCenterDocument::mark_dirty() {
    dirty_ = true;
}

void
ControlCenterDocument::set_dirty(bool dirty) {
    dirty_ = dirty;
}

bool
ControlCenterDocumentGeometryEqual::operator()(const ControlCenterDocument& first,
                                               const ControlCenterDocument& second) const {
    return same_geometry(first, second);
}
