/**
 * @file test_viz.cpp
 * @brief Tests for VizContext visualization context
 *
 * These tests require a display environment to run.
 * They will be automatically skipped in headless environments.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

// compat headers
#include <compat.h>
#include <viz/compat.h>

// import modules
import sim_estab;
import sim_estab.viz;

namespace utf = boost::unit_test;
using namespace sim_estab::viz;

//==============================================================================
// Display Detection
//==============================================================================

/**
 * @brief Check if a display is available for GUI tests
 *
 * Checks for X11 (DISPLAY) or Wayland (WAYLAND_DISPLAY) environment.
 */
static bool has_display_available() {
    const char *display = std::getenv("DISPLAY");
    const char *wayland = std::getenv("WAYLAND_DISPLAY");

    // No display environment variable set
    if (display == nullptr && wayland == nullptr) {
        return false;
    }

    return true;
}

/**
 * @brief Precondition predicate for display-requiring tests
 */
static boost::test_tools::assertion_result requires_display(utf::test_unit_id) {
    boost::test_tools::assertion_result result(has_display_available());
    if (!result) {
        result.message() << "Test skipped: no display available "
                         << "(set DISPLAY or WAYLAND_DISPLAY)";
    }
    return result;
}

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(viz_tests)

/**
 * @brief Test VizContext default construction
 */
BOOST_AUTO_TEST_CASE(test_viz_context_creation,
                     *utf::precondition(requires_display) *
                         utf::description("Test VizContext creation with default parameters")) {
    BOOST_TEST_MESSAGE("Creating VizContext with default parameters");

    VizContext ctx;

    BOOST_TEST(ctx.has_gpu_device());
    BOOST_TEST(ctx.get_width() == 1280);
    BOOST_TEST(ctx.get_height() == 720);

    // Show window for visual verification
    ctx.show();
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

/**
 * @brief Test VizContext with custom parameters
 */
BOOST_AUTO_TEST_CASE(test_viz_context_custom_params,
                     *utf::precondition(requires_display) *
                         utf::description("Test VizContext creation with custom parameters")) {
    BOOST_TEST_MESSAGE("Creating VizContext with custom parameters");

    VizContext ctx(800, 600, "Test Window", true);

    BOOST_TEST(ctx.has_gpu_device());
    BOOST_TEST(ctx.get_width() == 800);
    BOOST_TEST(ctx.get_height() == 600);
    BOOST_TEST(ctx.get_title() == "Test Window");
}

/**
 * @brief Test VizContext window visibility
 */
BOOST_AUTO_TEST_CASE(test_viz_context_visibility,
                     *utf::precondition(requires_display) * utf::description("Test VizContext show/hide operations")) {
    VizContext ctx(640, 480, "Visibility Test");

    // Initially hidden (created with SDL_WINDOW_HIDDEN)
    BOOST_TEST(!ctx.is_visible());

    ctx.show();
    BOOST_TEST(ctx.is_visible());

    ctx.hide();
    BOOST_TEST(!ctx.is_visible());
}

/**
 * @brief Test VizContext move semantics
 */
BOOST_AUTO_TEST_CASE(test_viz_context_move, *utf::precondition(requires_display) *
                                                utf::description("Test VizContext move constructor and assignment")) {
    VizContext ctx1(800, 600, "Original");
    BOOST_TEST(ctx1.has_gpu_device());

    // Move constructor
    VizContext ctx2(std::move(ctx1));
    BOOST_TEST(ctx2.has_gpu_device());
    BOOST_TEST(ctx2.get_width() == 800);

    // Move assignment
    VizContext ctx3(640, 480, "Target");
    ctx3 = std::move(ctx2);
    BOOST_TEST(ctx3.has_gpu_device());
    BOOST_TEST(ctx3.get_width() == 800);
}

/**
 * @brief Test VizContext GPU info retrieval
 */
BOOST_AUTO_TEST_CASE(test_viz_context_gpu_info,
                     *utf::precondition(requires_display) * utf::description("Test VizContext GPU device info")) {
    VizContext ctx;

    auto info = ctx.get_gpu_info();

    // Should have a valid backend (Vulkan or D3D12 on most systems)
    BOOST_TEST((info.backend == GPUBackend::Vulkan || info.backend == GPUBackend::D3D12 ||
                info.backend == GPUBackend::Unknown));
}

/**
 * @brief Test VizContext title modification
 */
BOOST_AUTO_TEST_CASE(test_viz_context_set_title,
                     *utf::precondition(requires_display) * utf::description("Test VizContext title modification")) {
    VizContext ctx(640, 480, "Initial Title");

    BOOST_TEST(ctx.get_title() == "Initial Title");

    ctx.set_title("New Title");
    BOOST_TEST(ctx.get_title() == "New Title");
}

BOOST_AUTO_TEST_SUITE_END()
