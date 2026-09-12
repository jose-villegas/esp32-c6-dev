extern "C" {
#include "ui/system_navigation.h"
}

#include <gtest/gtest.h>

namespace {

TEST(SystemNavigation, StartsOnLauncher) {
    system_navigation_t navigation;
    system_navigation_init(&navigation);

    EXPECT_EQ(navigation.screen, SYSTEM_SCREEN_LAUNCHER);
}

TEST(SystemNavigation, OpensFromTopEdgeSwipe) {
    system_navigation_t navigation;
    system_navigation_init(&navigation);
    const input_t input = {.down = true, .x = 100, .y = 120, .press_x = 100, .press_y = 10};

    EXPECT_TRUE(system_navigation_step(&navigation, &input, GESTURE_EDGE_TOP, GESTURE_EDGE_BOTTOM, 368, 448));
    EXPECT_EQ(navigation.screen, SYSTEM_SCREEN_CONTROL_CENTER);
}

TEST(SystemNavigation, IgnoresShortOrOffEdgeMovement) {
    system_navigation_t navigation;
    system_navigation_init(&navigation);
    input_t input = {.down = true, .x = 100, .y = 70, .press_x = 100, .press_y = 10};

    EXPECT_FALSE(system_navigation_step(&navigation, &input, GESTURE_EDGE_TOP, GESTURE_EDGE_BOTTOM, 368, 448));
    input.press_y = 80;
    input.y = 190;
    EXPECT_FALSE(system_navigation_step(&navigation, &input, GESTURE_EDGE_TOP, GESTURE_EDGE_BOTTOM, 368, 448));
    EXPECT_EQ(navigation.screen, SYSTEM_SCREEN_LAUNCHER);
}

TEST(SystemNavigation, ClosesWithSwipeUpFromBottom) {
    system_navigation_t navigation = {.screen = SYSTEM_SCREEN_CONTROL_CENTER};
    const input_t input = {.down = true, .x = 100, .y = 310, .press_x = 100, .press_y = 430};

    EXPECT_TRUE(system_navigation_step(&navigation, &input, GESTURE_EDGE_TOP, GESTURE_EDGE_BOTTOM, 368, 448));
    EXPECT_EQ(navigation.screen, SYSTEM_SCREEN_LAUNCHER);
}

TEST(SystemNavigation, HonorsRotatedPhysicalEdges) {
    system_navigation_t navigation;
    system_navigation_init(&navigation);
    const input_t input = {.down = true, .x = 320, .y = 100, .press_x = 440, .press_y = 100};

    EXPECT_TRUE(system_navigation_step(&navigation, &input, GESTURE_EDGE_RIGHT, GESTURE_EDGE_LEFT, 448, 368));
    EXPECT_EQ(navigation.screen, SYSTEM_SCREEN_CONTROL_CENTER);
}

} // namespace
