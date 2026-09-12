#include "control_center_document.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using testing::Contains;
using testing::HasSubstr;

std::size_t
index_of(ControlCenterElement element) {
    return static_cast<std::size_t>(element);
}

class TemporaryControlCenterLayout {
  public:
    TemporaryControlCenterLayout() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("control-center-layout-" + std::to_string(suffix) + ".json");
        std::filesystem::copy_file(ENGINE_TEST_CONTROL_CENTER_PATH, path_,
                                   std::filesystem::copy_options::overwrite_existing);
    }

    ~TemporaryControlCenterLayout() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path&
    path() const {
        return path_;
    }

    nlohmann::ordered_json
    read() const {
        std::ifstream stream(path_);
        return nlohmann::ordered_json::parse(stream);
    }

    void
    write(const nlohmann::ordered_json& source) const {
        std::ofstream stream(path_, std::ios::trunc);
        stream << source.dump(2) << '\n';
    }

  private:
    std::filesystem::path path_;
};

ControlCenterDocument
load_layout() {
    std::string error;
    std::optional<ControlCenterDocument> document = ControlCenterDocument::load(ENGINE_TEST_CONTROL_CENTER_PATH, error);
    EXPECT_TRUE(document.has_value()) << error;
    return std::move(document.value());
}

TEST(ControlCenterDocument, LoadsBothDeviceOrientations) {
    const ControlCenterDocument document = load_layout();

    EXPECT_TRUE(document.validate().empty());
    EXPECT_EQ(document.layout(LayoutOrientation::Landscape).canvas_width, 448);
    EXPECT_EQ(document.layout(LayoutOrientation::Portrait).canvas_height, 448);
    EXPECT_STREQ(control_center_element_id(ControlCenterElement::Wifi), "wifi");
    EXPECT_STREQ(control_center_element_label(ControlCenterElement::Brightness), "Brightness");
}

TEST(ControlCenterDocument, RejectsPanelOverlap) {
    ControlCenterDocument document = load_layout();
    ControlCenterLayout& layout = document.layout(LayoutOrientation::Landscape);
    layout.rects[index_of(ControlCenterElement::Bluetooth)] = layout.rects[index_of(ControlCenterElement::Wifi)];

    EXPECT_THAT(document.validate(), Contains("landscape: wifi overlaps bluetooth"));
}

TEST(ControlCenterDocument, RejectsUndersizedInteractiveTarget) {
    ControlCenterDocument document = load_layout();
    document.layout(LayoutOrientation::Portrait).rects[index_of(ControlCenterElement::Volume)].height = 43;

    EXPECT_THAT(document.validate(), Contains("portrait: volume is smaller than 44px"));
}

TEST(ControlCenterDocument, RejectsOutOfBoundsElement) {
    ControlCenterDocument document = load_layout();
    document.layout(LayoutOrientation::Portrait).rects[index_of(ControlCenterElement::Grabber)].x = -1;

    EXPECT_THAT(document.validate(), Contains("portrait: grabber leaves the canvas"));
}

TEST(ControlCenterDocument, ReportsMissingSource) {
    std::string error;
    EXPECT_FALSE(ControlCenterDocument::load("missing-control-center.json", error));
    EXPECT_THAT(error, HasSubstr("could not open"));
}

TEST(ControlCenterDocument, RoundTripsEditedGeometry) {
    TemporaryControlCenterLayout source;
    std::string error;
    std::optional<ControlCenterDocument> loaded = ControlCenterDocument::load(source.path(), error);
    ASSERT_TRUE(loaded.has_value()) << error;
    ControlCenterDocument document = std::move(*loaded);
    document.layout(LayoutOrientation::Landscape).rects[index_of(ControlCenterElement::Wifi)].x++;
    document.mark_dirty();

    ASSERT_TRUE(document.dirty());
    ASSERT_TRUE(document.save(error)) << error;
    EXPECT_FALSE(document.dirty());
    EXPECT_TRUE(error.empty());
    loaded = ControlCenterDocument::load(source.path(), error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->layout(LayoutOrientation::Landscape).rects[index_of(ControlCenterElement::Wifi)].x,
              document.layout(LayoutOrientation::Landscape).rects[index_of(ControlCenterElement::Wifi)].x);
}

TEST(ControlCenterDocument, RefusesToSaveInvalidGeometry) {
    ControlCenterDocument document = load_layout();
    document.layout(LayoutOrientation::Portrait).canvas_height = 1;

    std::string error;
    EXPECT_FALSE(document.save(error));
    EXPECT_THAT(error, HasSubstr("canvas must be 368 x 448"));
}

TEST(ControlCenterDocument, RejectsUnexpectedAndUnsupportedSchema) {
    TemporaryControlCenterLayout source;
    nlohmann::ordered_json json = source.read();
    json["unexpected"] = true;
    source.write(json);
    std::string error;
    EXPECT_FALSE(ControlCenterDocument::load(source.path(), error));
    EXPECT_THAT(error, HasSubstr("unexpected fields"));

    json.erase("unexpected");
    json["screen"] = "launcher";
    source.write(json);
    EXPECT_FALSE(ControlCenterDocument::load(source.path(), error));
    EXPECT_THAT(error, HasSubstr("unsupported Control Center document schema"));
}

TEST(ControlCenterDocument, RejectsElementOrderAndMalformedRectangles) {
    TemporaryControlCenterLayout source;
    nlohmann::ordered_json json = source.read();
    json["element_order"][0] = "wrong";
    source.write(json);
    std::string error;
    EXPECT_FALSE(ControlCenterDocument::load(source.path(), error));
    EXPECT_THAT(error, HasSubstr("element_order"));

    json = source.read();
    json["element_order"][0] = "grabber";
    json["orientations"]["portrait"]["rects"]["grabber"] = {1, 2, 3};
    source.write(json);
    EXPECT_FALSE(ControlCenterDocument::load(source.path(), error));
    EXPECT_THAT(error, HasSubstr("rectangle must be"));

    json["orientations"]["portrait"]["rects"]["grabber"] = {1, 2, "wide", 4};
    source.write(json);
    EXPECT_FALSE(ControlCenterDocument::load(source.path(), error));
    EXPECT_THAT(error, HasSubstr("components must be integers"));
}

TEST(ControlCenterDocument, HistoryTracksGeometryAndSavedRevision) {
    ControlCenterDocument document = load_layout();
    ControlCenterEditHistory history(document);
    document.layout(LayoutOrientation::Portrait).rects[index_of(ControlCenterElement::Brightness)].x--;
    history.commit(document);

    EXPECT_TRUE(document.dirty());
    ASSERT_TRUE(history.undo(document));
    EXPECT_FALSE(document.dirty());
    ASSERT_TRUE(history.redo(document));
    history.mark_saved(document);
    EXPECT_FALSE(document.dirty());
}

} // namespace
