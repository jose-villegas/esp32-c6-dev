#include "launcher_document.h"

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

using testing::HasSubstr;

std::size_t
index_of(LauncherElement element) {
    return static_cast<std::size_t>(element);
}

class TemporaryLayout {
  public:
    TemporaryLayout() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("engine-launcher-layout-" + std::to_string(suffix) + ".json");
        std::filesystem::copy_file(ENGINE_TEST_LAYOUT_PATH, path_, std::filesystem::copy_options::overwrite_existing);
    }

    ~TemporaryLayout() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path&
    path() const {
        return path_;
    }

    nlohmann::ordered_json
    read_json() const {
        std::ifstream stream(path_);
        return nlohmann::ordered_json::parse(stream);
    }

    void
    write_json(const nlohmann::ordered_json& source) const {
        std::ofstream stream(path_, std::ios::trunc);
        stream << source.dump(2) << '\n';
    }

  private:
    std::filesystem::path path_;
};

LauncherDocument
load_layout(const std::filesystem::path& path) {
    std::string error;
    std::optional<LauncherDocument> document = LauncherDocument::load(path, error);
    EXPECT_TRUE(document.has_value()) << error;
    return std::move(document.value());
}

TEST(LauncherDocument, LoadsCheckedInLayout) {
    LauncherDocument document = load_layout(ENGINE_TEST_LAYOUT_PATH);

    EXPECT_TRUE(document.validate().empty());
    EXPECT_EQ(document.layout(LauncherOrientation::Landscape).canvas_width, 448);
    EXPECT_EQ(document.layout(LauncherOrientation::Landscape).canvas_height, 368);
    EXPECT_EQ(document.layout(LauncherOrientation::Portrait).canvas_width, 368);
    EXPECT_EQ(document.layout(LauncherOrientation::Portrait).canvas_height, 448);
    EXPECT_EQ(document.path(), std::filesystem::path(ENGINE_TEST_LAYOUT_PATH));
    EXPECT_STREQ(launcher_element_id(LauncherElement::Library), "library");
    EXPECT_STREQ(launcher_element_label(LauncherElement::Library), "Library");
    EXPECT_STREQ(launcher_orientation_id(LauncherOrientation::Portrait), "portrait");
}

TEST(LauncherDocument, ReportsInvalidGeometry) {
    LauncherDocument document = load_layout(ENGINE_TEST_LAYOUT_PATH);
    LauncherLayout& landscape = document.layout(LauncherOrientation::Landscape);

    landscape.canvas_width = 400;
    landscape.rects[index_of(LauncherElement::LastPlayed)] = {-1, 98, 43, 20};
    landscape.rects[index_of(LauncherElement::Library)] = {10, 98, 125, 173};
    landscape.rects[index_of(LauncherElement::StatusBar)].height = 100;
    landscape.rects[index_of(LauncherElement::PageIndicator)].y = 100;

    const std::vector<std::string> problems = document.validate();
    EXPECT_THAT(problems, testing::Contains("landscape: canvas must be 448 x 368"));
    EXPECT_THAT(problems, testing::Contains("landscape: last_played leaves the canvas"));
    EXPECT_THAT(problems, testing::Contains("landscape: last_played is smaller than 44px"));
    EXPECT_THAT(problems, testing::Contains("landscape: last_played overlaps library"));
    EXPECT_THAT(problems, testing::Contains("landscape: status_bar overlaps the app region"));
    EXPECT_THAT(problems, testing::Contains("landscape: page_indicator must be below every app card"));
}

TEST(LauncherDocument, RejectsUnexpectedSchemaFields) {
    TemporaryLayout layout;
    nlohmann::ordered_json source = layout.read_json();
    source["unexpected"] = true;
    layout.write_json(source);

    std::string error;
    EXPECT_FALSE(LauncherDocument::load(layout.path(), error));
    EXPECT_THAT(error, HasSubstr("unexpected fields"));
}

TEST(LauncherDocument, RejectsMalformedAndUnsupportedDocuments) {
    TemporaryLayout layout;
    nlohmann::ordered_json source = layout.read_json();
    source["schema_version"] = 2;
    layout.write_json(source);

    std::string error;
    EXPECT_FALSE(LauncherDocument::load(layout.path(), error));
    EXPECT_THAT(error, HasSubstr("unsupported launcher document schema"));

    layout.write_json(nlohmann::ordered_json{{"schema_version", 1}});
    EXPECT_FALSE(LauncherDocument::load(layout.path(), error));
    EXPECT_THAT(error, HasSubstr("unexpected fields"));
}

TEST(LauncherDocument, RoundTripsSourceAndDirtyState) {
    TemporaryLayout layout;
    LauncherDocument document = load_layout(layout.path());
    document.layout(LauncherOrientation::Portrait).rects[index_of(LauncherElement::Library)].x++;
    document.mark_dirty();
    ASSERT_TRUE(document.dirty());

    std::string error;
    ASSERT_TRUE(document.save(error)) << error;
    EXPECT_FALSE(document.dirty());
    EXPECT_TRUE(error.empty());

    std::optional<LauncherDocument> reloaded = LauncherDocument::load(layout.path(), error);
    ASSERT_TRUE(reloaded.has_value()) << error;
    EXPECT_EQ(reloaded->layout(LauncherOrientation::Portrait).rects[index_of(LauncherElement::Library)].x,
              document.layout(LauncherOrientation::Portrait).rects[index_of(LauncherElement::Library)].x);
}

TEST(LauncherDocument, RefusesToSaveInvalidGeometry) {
    TemporaryLayout layout;
    LauncherDocument document = load_layout(layout.path());
    document.layout(LauncherOrientation::Portrait).canvas_width = 1;

    std::string error;
    EXPECT_FALSE(document.save(error));
    EXPECT_THAT(error, HasSubstr("canvas must be 368 x 448"));
}

TEST(LauncherDocument, ReportsMissingFile) {
    std::string error;
    EXPECT_FALSE(LauncherDocument::load("does-not-exist.json", error));
    EXPECT_THAT(error, HasSubstr("could not open"));
}

} // namespace
