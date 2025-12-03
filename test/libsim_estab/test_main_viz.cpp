/**
 * @file test_main_viz.cpp
 * @brief Main entry point for libsim_estab visualization tests
 *
 * This file serves as the main test entry point for visualization-related
 * libsim_estab C++ tests. These tests require a display environment.
 */

#define BOOST_TEST_MODULE libsim_estab_viz_tests

#include <boost/test/unit_test.hpp>

// Import the main SimEstab module
import sim_estab;
import sim_estab.viz;

// If macros are needed, include macro.h
#include <macro.h>

/**
 * @brief Global test suite initialization for visualization tests
 *
 * This function is called once before any tests are executed.
 * Use this for setup that needs to happen before all visualization tests.
 */
struct GlobalVizFixture {
    GlobalVizFixture() {
        // Initialize test environment
        BOOST_TEST_MESSAGE("Global visualization test setup started");

        // Add any global initialization code here
        BOOST_TEST_MESSAGE("Global visualization test setup completed");
    }

    ~GlobalVizFixture() {
        // Clean up test environment
        BOOST_TEST_MESSAGE("Global visualization test teardown started");

        // Add any global cleanup code here
        BOOST_TEST_MESSAGE("Global visualization test teardown completed");
    }
};

// Register the global fixture with Boost.Test
BOOST_TEST_GLOBAL_FIXTURE(GlobalVizFixture);
