/**
 * @file test_error.cpp
 * @brief Tests for the library's exception family
 *
 * Every subsystem error derives from sim_estab_error, so one catch clause covers
 * all of them (design_patterns.md §6).
 */

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>

// compat headers
#include <compat.h>

// import modules
import sim_estab;

namespace utf = boost::unit_test;
using sim_estab::core::error::sim_estab_error;
using sim_estab::core::gpu::gpu_error;
using sim_estab::core::log::log_error;

static_assert(std::is_base_of_v<std::runtime_error, sim_estab_error>);
static_assert(std::is_base_of_v<sim_estab_error, log_error>);
static_assert(std::is_base_of_v<sim_estab_error, gpu_error>);

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(error_tests)

/**
 * @brief A subsystem error is caught by a handler for the root
 */
BOOST_AUTO_TEST_CASE(test_root_catches_subsystem_errors,
                     *utf::description("a handler for sim_estab_error catches log_error and gpu_error")) {
    BOOST_CHECK_THROW(throw log_error("log failure"), sim_estab_error);
    BOOST_CHECK_THROW(throw gpu_error("gpu failure"), sim_estab_error);
}

/**
 * @brief The message survives the trip through the root
 */
BOOST_AUTO_TEST_CASE(test_root_keeps_message, *utf::description("what() is unchanged when caught as the root")) {
    try {
        throw gpu_error("gpu failure");
    } catch (const sim_estab_error& e) {
        BOOST_TEST(std::string(e.what()) == "gpu failure");
    }
}

BOOST_AUTO_TEST_SUITE_END()
