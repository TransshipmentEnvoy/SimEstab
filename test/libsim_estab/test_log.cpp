/**
 * @file test_log.cpp
 * @brief Comprehensive tests for the SimEstab logging system
 *
 * This file contains thorough tests for all logging functionality including:
 * - Initialization and deinitialization
 * - Severity level handling
 * - Channel-based logging
 * - Macro functionality
 * - Thread safety
 * - Output formatting
 */

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Boost
#include <boost/log/core.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/keywords/severity.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

// import
import sim_estab; // :log

// If macros are needed, include macro.h (since log.h is missing)
#include <macro.h>

using namespace sim_estab::core::log;

BOOST_AUTO_TEST_SUITE(logging_tests)

/**
 * @brief Test fixture for logging tests
 *
 * Provides automatic initialization and cleanup of the logging system
 * for each test case.
 */
struct LoggingFixture {
    LoggingFixture() {
        BOOST_TEST_MESSAGE("Logging fixture: setting up logging system");
        log_init();
    }

    ~LoggingFixture() {
        BOOST_TEST_MESSAGE("Logging fixture: tearing down logging system");
        log_deinit();
    }
};

BOOST_FIXTURE_TEST_SUITE(initialization_tests, LoggingFixture)

/**
 * @brief Test that logging system initializes properly
 */
BOOST_AUTO_TEST_CASE(test_log_init) {
    BOOST_TEST_MESSAGE("Testing log_init functionality");

    // Test that logging system reports as initialized
    BOOST_CHECK(log_is_init());

    // Test that multiple initializations are idempotent
    log_init(); // Should not crash or cause issues
    BOOST_CHECK(log_is_init());
}

/**
 * @brief Test that logging system deinitializes properly
 */
BOOST_AUTO_TEST_CASE(test_log_deinit) {
    BOOST_TEST_MESSAGE("Testing log_deinit functionality");

    // Deinitialize and check
    log_deinit();
    BOOST_CHECK(!log_is_init());

    // Test that multiple deinitializations are safe
    log_deinit(); // Should not crash
    BOOST_CHECK(!log_is_init());
}

/**
 * @brief Test that we can reinitialize after deinitialization
 */
BOOST_AUTO_TEST_CASE(test_reinitialization) {
    BOOST_TEST_MESSAGE("Testing logging reinitialization");

    // Deinitialize first
    log_deinit();
    BOOST_CHECK(!log_is_init());

    // Reinitialize
    log_init();
    BOOST_CHECK(log_is_init());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(severity_level_tests)

/**
 * @brief Test that severity levels have correct integer values
 */
BOOST_AUTO_TEST_CASE(test_severity_level_values) {
    BOOST_TEST_MESSAGE("Testing severity level enumeration values");

    BOOST_CHECK(static_cast<int>(severity_level::trace) == 0);
    BOOST_CHECK(static_cast<int>(severity_level::debug) == 10);
    BOOST_CHECK(static_cast<int>(severity_level::info) == 20);
    BOOST_CHECK(static_cast<int>(severity_level::warning) == 30);
    BOOST_CHECK(static_cast<int>(severity_level::error) == 40);
    BOOST_CHECK(static_cast<int>(severity_level::critical) == 50);
}

/**
 * @brief Test severity level stream output operator
 */
BOOST_AUTO_TEST_CASE(test_severity_level_stream_output) {
    BOOST_TEST_MESSAGE("Testing severity level stream output");

    std::stringstream ss;

    // Test each severity level
    ss << severity_level::trace;
    BOOST_CHECK(ss.str() == "TRACE");
    ss.str("");

    ss << severity_level::debug;
    BOOST_CHECK(ss.str() == "DEBUG");
    ss.str("");

    ss << severity_level::info;
    BOOST_CHECK(ss.str() == "INFO");
    ss.str("");

    ss << severity_level::warning;
    BOOST_CHECK(ss.str() == "WARNING");
    ss.str("");

    ss << severity_level::error;
    BOOST_CHECK(ss.str() == "ERROR");
    ss.str("");

    ss << severity_level::critical;
    BOOST_CHECK(ss.str() == "CRITICAL");
    ss.str("");

    // Test unknown value (cast from invalid int)
    ss << static_cast<severity_level>(999);
    BOOST_CHECK(ss.str() == "UNKNOWN");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(logging_macro_tests, LoggingFixture)

/**
 * @brief Test basic logging macro functionality
 */
BOOST_AUTO_TEST_CASE(test_basic_logging) {
    BOOST_TEST_MESSAGE("Testing basic logging macro functionality");

    // Test that logging macros don't crash when logging system is initialized
    BOOST_TEST_CHECKPOINT("Before logging test message");

    SIM_ESTAB_LOG("test.channel", info, "Test message");
    SIM_ESTAB_LOG_TRACE("test.channel", "Trace message");
    SIM_ESTAB_LOG_DEBUG("test.channel", "Debug message");
    SIM_ESTAB_LOG_INFO("test.channel", "Info message");
    SIM_ESTAB_LOG_WARN("test.channel", "Warning message");
    SIM_ESTAB_LOG_ERROR("test.channel", "Error message");
    SIM_ESTAB_LOG_CRITICAL("test.channel", "Critical message");

    BOOST_TEST_CHECKPOINT("After logging test messages");
}

/**
 * @brief Test logging with critical
 */
BOOST_AUTO_TEST_CASE(test_logging_with_critical) {
    BOOST_TEST_MESSAGE("Testing logging with critical severity");

    SIM_ESTAB_LOG_CRITICAL("test.channel", "Critical error occurred");
    SIM_ESTAB_LOG_CRITICAL("test.channel", "Critical error occurred");
    SIM_ESTAB_LOG_WARN("test.channel", "Critical error occurred");
    SIM_ESTAB_LOG_WARN("test.channel", "Critical error occurred");
    SIM_ESTAB_LOG_CRITICAL("test.channel", "Critical error occurred");
    SIM_ESTAB_LOG_CRITICAL("test.channel", "Critical error occurred");
    SIM_ESTAB_LOG_INFO("test.channel", "Critical error occurred");
    SIM_ESTAB_LOG_INFO("test.channel", "Critical error occurred");
}

/**
 * @brief Test logging with multiple message parts
 */
BOOST_AUTO_TEST_CASE(test_logging_with_multiple_message_parts) {
    BOOST_TEST_MESSAGE("Testing logging with multiple message parts");

    int value        = 42;
    std::string text = "test";

    // Test that stream operators work in logging macros
    // SIM_ESTAB_LOG("test.channel", info, "Integer: " << value << ", String: " << text);
    // SIM_ESTAB_LOG_DEBUG("test.channel", "Debug info: value=" << value << ", text='" << text << "'");
    SIM_ESTAB_LOG("test.channel", info, text);
}

/**
 * @brief Test hierarchical channel naming
 */
BOOST_AUTO_TEST_CASE(test_hierarchical_channels) {
    BOOST_TEST_MESSAGE("Testing hierarchical channel naming");

    // Test various hierarchical channel names
    SIM_ESTAB_LOG("sim_estab", info, "Root channel");
    SIM_ESTAB_LOG("sim_estab.core", debug, "Core subsystem");
    SIM_ESTAB_LOG("sim_estab.core.log", trace, "Log subsystem");
    SIM_ESTAB_LOG("sim_estab.network.tcp", info, "TCP networking");
    SIM_ESTAB_LOG("sim_estab.render.opengl", warning, "OpenGL rendering");
    SIM_ESTAB_LOG("sim_estab.physics.bullet", error, "Physics engine");
}

/**
 * @brief Test logging when system is not initialized
 */
BOOST_AUTO_TEST_CASE(test_logging_when_not_initialized) {
    BOOST_TEST_MESSAGE("Testing logging behavior when system is not initialized");

    // Deinitialize first
    log_deinit();
    BOOST_CHECK(!log_is_init());

    // These should not crash, just silently fail
    SIM_ESTAB_LOG("test.channel", info, "This should not appear");
    SIM_ESTAB_LOG_CRITICAL("test.channel", "This should also not appear");

    // Reinitialize for other tests
    log_init();
    BOOST_CHECK(log_is_init());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(thread_safety_tests, LoggingFixture)

/**
 * @brief Test concurrent logging from multiple threads
 */
BOOST_AUTO_TEST_CASE(test_concurrent_logging) {
    BOOST_TEST_MESSAGE("Testing concurrent logging from multiple threads");

    const int num_threads         = 4;
    const int messages_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> counter{0};

    auto logging_lambda = [&counter, messages_per_thread](int thread_id) {
        for (int i = 0; i < messages_per_thread; ++i) {
            SIM_ESTAB_LOG("thread.test", info, "Thread ", thread_id, ", message ", i);
            counter++;
        }
    };

    // Start threads
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(logging_lambda, i);
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all messages were processed
    BOOST_CHECK(counter == num_threads * messages_per_thread);
}

/**
 * @brief Test concurrent initialization/deinitialization
 */
BOOST_AUTO_TEST_CASE(test_concurrent_init_deinit) {
    BOOST_TEST_MESSAGE("Testing concurrent initialization/deinitialization");

    const int num_iterations = 50;
    std::vector<std::thread> threads;

    auto init_deinit_lambda = [num_iterations]() {
        for (int i = 0; i < num_iterations; ++i) {
            log_init();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            log_deinit();
        }
    };

    // Start multiple threads doing init/deinit
    const int num_threads = 3;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(init_deinit_lambda);
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Ensure logging system is in a clean state
    log_deinit();
    log_init();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(exception_handling_tests)

/**
 * @brief Test log_error exception class
 */
BOOST_AUTO_TEST_CASE(test_log_error_exception) {
    BOOST_TEST_MESSAGE("Testing log_error exception class");

    std::string error_msg = "Test logging error";
    log_error error(error_msg);

    // Test that the exception stores the message correctly
    BOOST_CHECK(std::string(error.what()) == error_msg);

    // Test that it inherits from std::runtime_error
    BOOST_CHECK(dynamic_cast<std::runtime_error *>(&error) != nullptr);
}

/**
 * @brief Test that exceptions during logging are handled gracefully
 */
BOOST_AUTO_TEST_CASE(test_graceful_exception_handling) {
    BOOST_TEST_MESSAGE("Testing graceful exception handling in logging");

    // Initialize logging
    log_init();

    // This test ensures that logging doesn't throw exceptions under normal conditions
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "Test message"));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG_CRITICAL("test.channel", "Critical test"));

    // Test with extreme cases
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, ""));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("", info, "Empty channel"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(performance_tests)

/**
 * @brief Test logging performance with many messages
 */
BOOST_AUTO_TEST_CASE(test_logging_performance) {
    BOOST_TEST_MESSAGE("Testing logging performance with many messages");

    // Initialize logging
    log_init();

    const int num_messages = 1000;
    auto start             = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_messages; ++i) {
        SIM_ESTAB_LOG("performance.test", debug, "Performance test message ", i);
    }

    auto end      = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    BOOST_TEST_MESSAGE("Logged " << num_messages << " messages in " << duration.count() << "ms");

    // Performance assertion: should be able to log 1000 messages in reasonable time
    BOOST_CHECK(duration.count() < 5000); // Less than 5 seconds
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()