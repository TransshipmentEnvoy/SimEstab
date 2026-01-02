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

    // Poll events and present frames for ~2 seconds (required for Wayland compositing)
    for (int i = 0; i < 120; ++i) {
        ctx.poll_events();
        ctx.clear(); // Black background
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

/**
 * @brief Test VizContext with custom parameters
 */
BOOST_AUTO_TEST_CASE(test_viz_context_custom_params,
                     *utf::precondition(requires_display) *
                         utf::description("Test VizContext creation with custom parameters")) {
    BOOST_TEST_MESSAGE("Creating VizContext with custom parameters");

    VizContext ctx(800, 600, "Test Window", true);
    ctx.clear();
    ctx.poll_events();

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
    ctx.clear();
    ctx.poll_events();

    // Initially hidden (created with SDL_WINDOW_HIDDEN)
    BOOST_TEST(!ctx.is_visible());

    ctx.show();
    BOOST_TEST(ctx.is_visible());

    ctx.hide();
    BOOST_TEST(!ctx.is_visible());
}

/**
 * @brief Test VizContext GPU info retrieval
 */
BOOST_AUTO_TEST_CASE(test_viz_context_gpu_info,
                     *utf::precondition(requires_display) * utf::description("Test VizContext GPU device info")) {
    VizContext ctx;
    ctx.clear();
    ctx.poll_events();

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
    ctx.clear();
    ctx.poll_events();

    BOOST_TEST(ctx.get_title() == "Initial Title");

    ctx.set_title("New Title");
    BOOST_TEST(ctx.get_title() == "New Title");
}

/**
 * @brief Test VizContext sequential creation and destruction
 *
 * This test verifies that VizContext instances can be created and destroyed
 * in sequence, which is the recommended usage pattern since SDL3 GPU API
 * may not support multiple simultaneous GPU devices on all systems.
 */
BOOST_AUTO_TEST_CASE(test_viz_context_sequential_lifecycle,
                     *utf::precondition(requires_display) *
                         utf::description("Test VizContext can be created and destroyed sequentially")) {
    BOOST_TEST_MESSAGE("Testing sequential VizContext lifecycle");

    // Create and destroy contexts in sequence
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST_MESSAGE("Creating VizContext iteration " << i);

        VizContext ctx(640 + i * 100, 480 + i * 50, "Sequential Window " + std::to_string(i));

        BOOST_REQUIRE(ctx.has_gpu_device());
        BOOST_TEST(ctx.get_width() == 640 + i * 100);
        BOOST_TEST(ctx.get_height() == 480 + i * 50);

        ctx.show();
        ctx.clear(static_cast<float>(i) / 3.0f, 0.5f, 1.0f - static_cast<float>(i) / 3.0f);
        ctx.poll_events();

        BOOST_TEST(ctx.is_visible());

        // Brief display
        for (int j = 0; j < 10; ++j) {
            ctx.poll_events();
            ctx.clear(static_cast<float>(i) / 3.0f, 0.5f, 1.0f - static_cast<float>(i) / 3.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        // Test title modification
        ctx.set_title("Modified " + std::to_string(i));
        BOOST_TEST(ctx.get_title() == "Modified " + std::to_string(i));

        // ctx is destroyed here at end of loop iteration
        BOOST_TEST_MESSAGE("VizContext iteration " << i << " completed");
    }

    BOOST_TEST_MESSAGE("Sequential VizContext lifecycle test completed");
}

/**
 * @brief Test VizContext GPU info retrieval after recreation
 *
 * Tests that GPU info can be retrieved correctly across multiple
 * sequential VizContext instances.
 */
BOOST_AUTO_TEST_CASE(test_viz_context_gpu_info_sequential,
                     *utf::precondition(requires_display) *
                         utf::description("Test GPU info retrieval across sequential VizContext instances")) {
    BOOST_TEST_MESSAGE("Testing GPU info from sequential contexts");

    GPUBackend first_backend = GPUBackend::Unknown;

    for (int i = 0; i < 2; ++i) {
        VizContext ctx(640, 480, "GPU Info Test " + std::to_string(i));
        BOOST_REQUIRE(ctx.has_gpu_device());
        ctx.clear();
        ctx.poll_events();

        auto info = ctx.get_gpu_info();

        // Should report a valid GPU backend
        BOOST_TEST((info.backend == GPUBackend::Vulkan || info.backend == GPUBackend::D3D12 ||
                    info.backend == GPUBackend::Unknown));

        if (i == 0) {
            first_backend = info.backend;
        } else {
            // Sequential contexts on the same system should use the same backend
            BOOST_TEST(static_cast<int>(info.backend) == static_cast<int>(first_backend));
        }

        BOOST_TEST_MESSAGE("GPU info iteration " << i << " completed");
    }

    BOOST_TEST_MESSAGE("Sequential GPU info test completed");
}

//==============================================================================
// Multiple Simultaneous VizContext Tests
//==============================================================================

/**
 * @brief Test multiple VizContext instances existing simultaneously
 *
 * This test verifies that multiple VizContext instances can coexist,
 * sharing the same underlying GPU device via the shared device management.
 */
BOOST_AUTO_TEST_CASE(test_viz_context_multiple_simultaneous,
                     *utf::precondition(requires_display) *
                         utf::description("Test multiple VizContext instances existing simultaneously")) {
    BOOST_TEST_MESSAGE("Testing multiple simultaneous VizContext instances");

    // Create two contexts at the same time
    VizContext ctx1(640, 480, "Window 1");
    BOOST_REQUIRE(ctx1.has_gpu_device());
    BOOST_TEST_MESSAGE("First VizContext created");

    VizContext ctx2(800, 600, "Window 2");
    BOOST_REQUIRE(ctx2.has_gpu_device());
    BOOST_TEST_MESSAGE("Second VizContext created");

    // Both should be functional
    BOOST_TEST(ctx1.get_width() == 640);
    BOOST_TEST(ctx1.get_height() == 480);
    BOOST_TEST(ctx2.get_width() == 800);
    BOOST_TEST(ctx2.get_height() == 600);

    // Show both windows
    ctx1.show();
    ctx2.show();

    // Both should report same GPU backend (they share the same device)
    auto info1 = ctx1.get_gpu_info();
    auto info2 = ctx2.get_gpu_info();
    BOOST_TEST(static_cast<int>(info1.backend) == static_cast<int>(info2.backend));

    // Render to both windows
    for (int i = 0; i < 30; ++i) {
        ctx1.poll_events();
        ctx2.poll_events();

        // Different colors for each window
        ctx1.clear(1.0f, 0.0f, 0.0f); // Red
        ctx2.clear(0.0f, 0.0f, 1.0f); // Blue

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    BOOST_TEST_MESSAGE("Multiple simultaneous VizContext test completed");
}

/**
 * @brief Test three VizContext instances with different sizes
 *
 * Tests that three windows can coexist and render independently.
 */
BOOST_AUTO_TEST_CASE(test_viz_context_three_windows,
                     *utf::precondition(requires_display) *
                         utf::description("Test three VizContext instances coexisting")) {
    BOOST_TEST_MESSAGE("Testing three simultaneous VizContext instances");

    VizContext ctx1(400, 300, "Small Window");
    VizContext ctx2(640, 480, "Medium Window");
    VizContext ctx3(800, 600, "Large Window");

    BOOST_REQUIRE(ctx1.has_gpu_device());
    BOOST_REQUIRE(ctx2.has_gpu_device());
    BOOST_REQUIRE(ctx3.has_gpu_device());

    ctx1.show();
    ctx2.show();
    ctx3.show();

    // Render different colors to each
    for (int i = 0; i < 30; ++i) {
        ctx1.poll_events();
        ctx2.poll_events();
        ctx3.poll_events();

        ctx1.clear(1.0f, 0.0f, 0.0f); // Red
        ctx2.clear(0.0f, 1.0f, 0.0f); // Green
        ctx3.clear(0.0f, 0.0f, 1.0f); // Blue

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    BOOST_TEST_MESSAGE("Three VizContext windows test completed");
}

/**
 * @brief Test creating and destroying VizContext while others exist
 *
 * Tests dynamic creation/destruction while other contexts remain active.
 */
BOOST_AUTO_TEST_CASE(test_viz_context_dynamic_lifecycle,
                     *utf::precondition(requires_display) *
                         utf::description("Test dynamic VizContext creation/destruction")) {
    BOOST_TEST_MESSAGE("Testing dynamic VizContext lifecycle");

    // Create persistent context
    VizContext persistent_ctx(640, 480, "Persistent Window");
    BOOST_REQUIRE(persistent_ctx.has_gpu_device());
    persistent_ctx.show();

    // Create and destroy temporary contexts while persistent one exists
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST_MESSAGE("Dynamic iteration " << i);

        VizContext temp_ctx(400, 300, "Temp Window " + std::to_string(i));
        BOOST_REQUIRE(temp_ctx.has_gpu_device());
        temp_ctx.show();

        // Render to both
        for (int j = 0; j < 15; ++j) {
            persistent_ctx.poll_events();
            temp_ctx.poll_events();

            persistent_ctx.clear(0.2f, 0.2f, 0.2f); // Dark gray
            temp_ctx.clear(static_cast<float>(i) / 3.0f, 0.5f, 1.0f);

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        // temp_ctx destroyed here
    }

    // Persistent context should still work
    BOOST_REQUIRE(persistent_ctx.has_gpu_device());
    persistent_ctx.clear(0.0f, 1.0f, 0.0f); // Green to confirm still working
    persistent_ctx.poll_events();

    BOOST_TEST_MESSAGE("Dynamic VizContext lifecycle test completed");
}

/**
 * @brief Test VizContext visibility operations with multiple windows
 */
BOOST_AUTO_TEST_CASE(test_viz_context_multi_visibility,
                     *utf::precondition(requires_display) *
                         utf::description("Test show/hide with multiple VizContext instances")) {
    BOOST_TEST_MESSAGE("Testing visibility with multiple contexts");

    VizContext ctx1(400, 300, "Visibility Test 1");
    VizContext ctx2(400, 300, "Visibility Test 2");

    BOOST_REQUIRE(ctx1.has_gpu_device());
    BOOST_REQUIRE(ctx2.has_gpu_device());

    // Both initially hidden
    BOOST_TEST(!ctx1.is_visible());
    BOOST_TEST(!ctx2.is_visible());

    // Show first
    ctx1.show();
    BOOST_TEST(ctx1.is_visible());
    BOOST_TEST(!ctx2.is_visible());

    // Show second
    ctx2.show();
    BOOST_TEST(ctx1.is_visible());
    BOOST_TEST(ctx2.is_visible());

    // Hide first
    ctx1.hide();
    BOOST_TEST(!ctx1.is_visible());
    BOOST_TEST(ctx2.is_visible());

    // Render to visible window
    for (int i = 0; i < 15; ++i) {
        ctx1.poll_events();
        ctx2.poll_events();
        ctx2.clear(0.5f, 0.5f, 1.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    BOOST_TEST_MESSAGE("Multi-visibility test completed");
}

BOOST_AUTO_TEST_SUITE_END()
