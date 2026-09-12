#include "launcher_document.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::ordered_json;

constexpr std::array<LauncherElement, static_cast<std::size_t>(LauncherElement::Count)> elements = {
    LauncherElement::StatusBar, LauncherElement::LastPlayed,    LauncherElement::Library,
    LauncherElement::RenderLab, LauncherElement::PageIndicator,
};

constexpr std::array<LauncherElement, 3> cards = {
    LauncherElement::LastPlayed,
    LauncherElement::Library,
    LauncherElement::RenderLab,
};

std::size_t
index_of(LauncherElement element) {
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

LauncherRect
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

LauncherLayout
read_layout(const Json& value) {
    require_exact_keys(value, {"canvas", "rects"}, "orientation");
    const Json& canvas = value.at("canvas");
    if (!canvas.is_array() || canvas.size() != 2 || !canvas[0].is_number_integer() || !canvas[1].is_number_integer()) {
        throw std::runtime_error("canvas must be [width, height]");
    }

    LauncherLayout layout = {canvas[0].get<int>(), canvas[1].get<int>(), {}};
    const Json& rects = value.at("rects");
    if (!rects.is_object() || rects.size() != elements.size()) {
        throw std::runtime_error("rects must contain exactly the stable launcher IDs");
    }
    for (LauncherElement element : elements) {
        if (!rects.contains(launcher_element_id(element))) {
            throw std::runtime_error(std::string("rects is missing ") + launcher_element_id(element));
        }
        layout.rects[index_of(element)] = read_rect(rects.at(launcher_element_id(element)));
    }
    return layout;
}

bool
overlaps(const LauncherRect& first, const LauncherRect& second) {
    return first.x < second.x + second.width && second.x < first.x + first.width && first.y < second.y + second.height
           && second.y < first.y + first.height;
}

void
validate_layout(const LauncherLayout& layout, LauncherOrientation orientation, std::vector<std::string>& problems) {
    const std::string prefix = std::string(launcher_orientation_id(orientation)) + ": ";
    for (LauncherElement element : elements) {
        const LauncherRect& rect = layout.rects[index_of(element)];
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 || rect.x + rect.width > layout.canvas_width
            || rect.y + rect.height > layout.canvas_height) {
            problems.push_back(prefix + launcher_element_id(element) + " leaves the canvas");
        }
    }

    for (std::size_t first = 0; first < cards.size(); first++) {
        const LauncherRect& first_rect = layout.rects[index_of(cards[first])];
        if (first_rect.width < 44 || first_rect.height < 44) {
            problems.push_back(prefix + launcher_element_id(cards[first]) + " is smaller than 44px");
        }
        for (std::size_t second = first + 1; second < cards.size(); second++) {
            if (overlaps(first_rect, layout.rects[index_of(cards[second])])) {
                problems.push_back(prefix + launcher_element_id(cards[first]) + " overlaps "
                                   + launcher_element_id(cards[second]));
            }
        }
    }

    const LauncherRect& status = layout.rects[index_of(LauncherElement::StatusBar)];
    int cards_top = layout.canvas_height;
    for (LauncherElement card : cards) {
        cards_top = std::min(cards_top, layout.rects[index_of(card)].y);
    }
    if (status.y + status.height > cards_top) {
        problems.push_back(prefix + "status_bar overlaps the app region");
    }

    int cards_bottom = 0;
    for (LauncherElement card : cards) {
        const LauncherRect& rect = layout.rects[index_of(card)];
        cards_bottom = std::max(cards_bottom, rect.y + rect.height);
    }
    if (layout.rects[index_of(LauncherElement::PageIndicator)].y < cards_bottom) {
        problems.push_back(prefix + "page_indicator must be below every app card");
    }
}

Json
write_layout(const LauncherLayout& layout) {
    Json rects = Json::object();
    for (LauncherElement element : elements) {
        const LauncherRect& rect = layout.rects[index_of(element)];
        rects[launcher_element_id(element)] = {rect.x, rect.y, rect.width, rect.height};
    }
    return {
        {"canvas", {layout.canvas_width, layout.canvas_height}},
        {"rects", std::move(rects)},
    };
}

} // namespace

const char*
launcher_element_id(LauncherElement element) {
    static constexpr std::array<const char*, static_cast<std::size_t>(LauncherElement::Count)> ids = {
        "status_bar", "last_played", "library", "render_lab", "page_indicator",
    };
    return ids.at(index_of(element));
}

const char*
launcher_element_label(LauncherElement element) {
    static constexpr std::array<const char*, static_cast<std::size_t>(LauncherElement::Count)> labels = {
        "Status bar", "Last played", "Library", "Render lab", "Page indicator",
    };
    return labels.at(index_of(element));
}

const char*
launcher_orientation_id(LauncherOrientation orientation) {
    return orientation == LauncherOrientation::Landscape ? "landscape" : "portrait";
}

std::optional<LauncherDocument>
LauncherDocument::load(const std::filesystem::path& path, std::string& error) {
    try {
        std::ifstream stream(path);
        if (!stream) {
            throw std::runtime_error("could not open " + path.string());
        }

        Json source;
        stream >> source;
        require_exact_keys(source, {"schema_version", "screen", "element_order", "orientations"}, "document");
        if (source.at("schema_version") != 1 || source.at("screen") != "launcher") {
            throw std::runtime_error("unsupported launcher document schema");
        }

        Json expected_order = Json::array();
        for (LauncherElement element : elements) {
            expected_order.push_back(launcher_element_id(element));
        }
        if (source.at("element_order") != expected_order) {
            throw std::runtime_error("element_order does not match the launcher schema");
        }

        const Json& orientations = source.at("orientations");
        require_exact_keys(orientations, {"portrait", "landscape"}, "orientations");

        LauncherDocument document;
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
LauncherDocument::save(std::string& error) {
    const std::vector<std::string> problems = validate();
    if (!problems.empty()) {
        error = problems.front();
        return false;
    }

    Json order = Json::array();
    for (LauncherElement element : elements) {
        order.push_back(launcher_element_id(element));
    }
    const Json source = {
        {"schema_version", 1},
        {"screen", "launcher"},
        {"element_order", std::move(order)},
        {"orientations",
         {
             {"portrait", write_layout(portrait_)},
             {"landscape", write_layout(landscape_)},
         }},
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
LauncherDocument::validate() const {
    std::vector<std::string> problems;
    if (portrait_.canvas_width != 368 || portrait_.canvas_height != 448) {
        problems.emplace_back("portrait: canvas must be 368 x 448");
    }
    if (landscape_.canvas_width != 448 || landscape_.canvas_height != 368) {
        problems.emplace_back("landscape: canvas must be 448 x 368");
    }
    validate_layout(portrait_, LauncherOrientation::Portrait, problems);
    validate_layout(landscape_, LauncherOrientation::Landscape, problems);
    return problems;
}

LauncherLayout&
LauncherDocument::layout(LauncherOrientation orientation) {
    return orientation == LauncherOrientation::Landscape ? landscape_ : portrait_;
}

const LauncherLayout&
LauncherDocument::layout(LauncherOrientation orientation) const {
    return orientation == LauncherOrientation::Landscape ? landscape_ : portrait_;
}

const std::filesystem::path&
LauncherDocument::path() const {
    return path_;
}

bool
LauncherDocument::dirty() const {
    return dirty_;
}

void
LauncherDocument::mark_dirty() {
    dirty_ = true;
}
