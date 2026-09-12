#pragma once

#include <array>
#include <cstddef>

enum class LayoutOrientation {
    Portrait,
    Landscape,
};

struct LayoutRect {
    int x;
    int y;
    int width;
    int height;
};

template <std::size_t ElementCount>
struct FixedLayout {
    int canvas_width;
    int canvas_height;
    std::array<LayoutRect, ElementCount> rects;
};

inline const char*
layout_orientation_id(LayoutOrientation orientation) {
    return orientation == LayoutOrientation::Landscape ? "landscape" : "portrait";
}
