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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// import
#include <compat.h>

import sim_estab; // :log

// If macros are needed, include macro.h
#include <macro.h>

using namespace sim_estab::core::log;

namespace test_log_types {

struct DirectRecordPrintable {
    int value{};
};

inline sim_estab::core::log::record_ostream& operator<<(sim_estab::core::log::record_ostream& os,
                                                        const DirectRecordPrintable& printable) {
    return os << "DirectRecordPrintable(" << printable.value << ")";
}

struct OStreamPrintable {
    int value{};
};

inline std::ostream& operator<<(std::ostream& os, const OStreamPrintable& printable) {
    return os << "OStreamPrintable(" << printable.value << ")";
}

SIM_ESTAB_LOG_ENABLE_OSTREAM(OStreamPrintable)

} // namespace test_log_types

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
    SIM_ESTAB_LOG("test.channel", info, "Integer: ", value, ", String: ", text);
}

/**
 * @brief Test logging with standard stream manipulators
 */
BOOST_AUTO_TEST_CASE(test_logging_with_stream_manipulators) {
    BOOST_TEST_MESSAGE("Testing logging with standard stream manipulators");

    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "hex=", std::hex, 255));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "bool=", std::boolalpha, true));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "precision=", std::setprecision(3), 1.23456));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "width=", std::setfill('0'), std::setw(4), 7));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, std::setiosflags(std::ios_base::showbase), std::hex, 255));
    BOOST_CHECK_NO_THROW(
        SIM_ESTAB_LOG("test.channel", info, std::resetiosflags(std::ios_base::showbase), std::setbase(10), 255));

    const std::tm timestamp{};
    const std::string amount = "1234";
    std::string quoted_text  = "quoted value";
    constexpr std::string_view quoted_view{"view value"};

    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "money=", std::put_money(1234.0L)));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "money=", std::put_money(amount)));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "time=", std::put_time(&timestamp, "%Y-%m-%d")));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "quoted=", std::quoted("literal")));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "quoted=", std::quoted(std::as_const(quoted_text))));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "quoted=", std::quoted(quoted_text)));
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, "quoted=", std::quoted(quoted_view)));
}

/**
 * @brief Test custom record_ostream output support
 */
BOOST_AUTO_TEST_CASE(test_logging_with_custom_record_ostream_type) {
    BOOST_TEST_MESSAGE("Testing logging with custom record_ostream output");

    test_log_types::DirectRecordPrintable printable{42};
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, printable));
}

/**
 * @brief Test explicit std::ostream adapter support
 */
BOOST_AUTO_TEST_CASE(test_logging_with_explicit_ostream_adapter) {
    BOOST_TEST_MESSAGE("Testing logging with explicit std::ostream adapter");

    test_log_types::OStreamPrintable printable{84};
    BOOST_CHECK_NO_THROW(SIM_ESTAB_LOG("test.channel", info, printable));
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

// ============================================================================
// Names from the enums (design_logging.md §3 "Names from the enums")
// ============================================================================

BOOST_AUTO_TEST_SUITE(enum_name_tests)

BOOST_AUTO_TEST_CASE(test_the_name_tables_follow_the_enums) {
    BOOST_TEST(severity_level_names.size() == 6);
    BOOST_TEST((severity_level_names[0].first == severity_level::trace));
    BOOST_TEST(severity_level_names[0].second == "trace");
    BOOST_TEST((severity_level_names[5].first == severity_level::critical));
    BOOST_TEST(severity_level_names[5].second == "critical");

    BOOST_TEST(file_open_names.size() == 3);
    BOOST_TEST(file_open_names[0].second == "rotate");
    BOOST_TEST(file_open_names[1].second == "append");
    BOOST_TEST(file_open_names[2].second == "truncate");
}

BOOST_AUTO_TEST_CASE(test_name_of_and_from_name) {
    static_assert(name_of(severity_level::warning) == "warning");
    static_assert(from_name<severity_level>("error") == severity_level::error);
    static_assert(from_name<file_open>("append") == file_open::append);
    BOOST_TEST(name_of(file_open::truncate) == "truncate");
    BOOST_TEST(!from_name<severity_level>("bogus").has_value());
    BOOST_TEST(name_of(static_cast<severity_level>(7)) == "unknown");
}

BOOST_AUTO_TEST_CASE(test_the_line_prints_the_upper_case_severity) {
    std::ostringstream os;
    os << severity_level::warning << ' ' << file_open::rotate;
    BOOST_TEST(os.str() == "WARNING rotate");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// File sinks and the severity filter (design_logging.md §3)
// ============================================================================

BOOST_AUTO_TEST_SUITE(file_sink_tests)

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

/// A fresh directory per test, removed at the end. Logging is initialized with the
/// console off, so only the files see the records; the console tests capture std::clog.
struct FileSinkFixture {
    fs::path dir;

    FileSinkFixture() {
        static int counter = 0;
        const auto stamp   = std::chrono::steady_clock::now().time_since_epoch().count();
        dir                = fs::temp_directory_path() /
              ("sim_estab_log_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        fs::create_directories(dir);
        log_init();
        disable_console();
    }

    ~FileSinkFixture() {
        log_deinit();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    fs::path file(const std::string& name = "test.log") const { return dir / name; }
};

/// std::clog captured for the fixture's life
struct CapturedConsole {
    std::ostringstream captured;
    std::streambuf *previous;
    CapturedConsole() : previous(std::clog.rdbuf(captured.rdbuf())) {}
    ~CapturedConsole() { std::clog.rdbuf(previous); }
    std::string text() const { return captured.str(); }
};

BOOST_FIXTURE_TEST_SUITE(file_sink_behaviour, FileSinkFixture)

BOOST_AUTO_TEST_CASE(test_enable_file_writes_the_records) {
    enable_file(file());
    sim_estab_log("test.file", severity_level::info, "hello file");
    disable_file(file());

    BOOST_TEST(contains(read_file(file()), "[INFO][test.file] hello file"));
}

BOOST_AUTO_TEST_CASE(test_the_band_filters_a_file) {
    enable_file(file(), severity_level::warning, severity_level::error);
    sim_estab_log("test.file", severity_level::info, "below the floor");
    sim_estab_log("test.file", severity_level::warning, "inside the band");
    sim_estab_log("test.file", severity_level::critical, "above the ceiling");
    disable_file(file());

    const std::string text = read_file(file());
    BOOST_TEST(!contains(text, "below the floor"));
    BOOST_TEST(contains(text, "inside the band"));
    BOOST_TEST(!contains(text, "above the ceiling"));
}

BOOST_AUTO_TEST_CASE(test_a_floor_below_the_users_level_reaches_the_file_only) {
    CapturedConsole console;
    enable_console();
    set_severity(severity_level::warning);
    enable_file(file(), severity_level::debug);
    sim_estab_log("test.file", severity_level::debug, "deep record");
    sim_estab_log("test.file", severity_level::warning, "loud record");
    disable_file(file());

    const std::string text = read_file(file());
    BOOST_TEST(contains(text, "deep record"));
    BOOST_TEST(contains(text, "loud record"));
    BOOST_TEST(!contains(console.text(), "deep record"));
    BOOST_TEST(contains(console.text(), "loud record"));
}

BOOST_AUTO_TEST_CASE(test_set_severity_moves_the_console_and_never_a_band) {
    CapturedConsole console;
    enable_console();
    enable_file(file(), severity_level::info);

    set_severity(severity_level::error);
    sim_estab_log("test.file", severity_level::warning, "quiet warning");
    set_severity(severity_level::trace);
    sim_estab_log("test.file", severity_level::debug, "trace-time debug");
    sim_estab_log("test.file", severity_level::info, "trace-time info");
    disable_file(file());

    BOOST_TEST(!contains(console.text(), "quiet warning"));
    BOOST_TEST(contains(console.text(), "trace-time debug"));
    const std::string text = read_file(file());
    BOOST_TEST(contains(text, "quiet warning"));     // the file's floor is info, whatever the console does
    BOOST_TEST(!contains(text, "trace-time debug")); // the band is fixed at the open
    BOOST_TEST(contains(text, "trace-time info"));
}

BOOST_AUTO_TEST_CASE(test_rotate_shuffles_the_backups) {
    for (const char *run : {"first run", "second run", "third run"}) {
        enable_file(file());
        sim_estab_log("test.file", severity_level::info, run);
        disable_file(file());
    }
    BOOST_TEST(contains(read_file(file("test.log")), "third run"));
    BOOST_TEST(contains(read_file(file("test.01.log")), "second run"));
    BOOST_TEST(contains(read_file(file("test.02.log")), "first run"));
    BOOST_TEST(!fs::exists(file("test.03.log")));
}

BOOST_AUTO_TEST_CASE(test_append_and_truncate_leave_the_backups_alone) {
    enable_file(file());
    sim_estab_log("test.file", severity_level::info, "first run");
    disable_file(file());
    enable_file(file());
    sim_estab_log("test.file", severity_level::info, "second run");
    disable_file(file());

    enable_file(file(), severity_level::trace, severity_level::critical, file_open::append);
    sim_estab_log("test.file", severity_level::info, "appended");
    disable_file(file());
    std::string text = read_file(file());
    BOOST_TEST(contains(text, "second run"));
    BOOST_TEST(contains(text, "appended"));
    BOOST_TEST(contains(read_file(file("test.01.log")), "first run"));
    BOOST_TEST(!fs::exists(file("test.02.log")));

    enable_file(file(), severity_level::trace, severity_level::critical, file_open::truncate);
    sim_estab_log("test.file", severity_level::info, "truncated");
    disable_file(file());
    text = read_file(file());
    BOOST_TEST(!contains(text, "second run"));
    BOOST_TEST(contains(text, "truncated"));
    BOOST_TEST(contains(read_file(file("test.01.log")), "first run"));
    BOOST_TEST(!fs::exists(file("test.02.log")));
}

BOOST_AUTO_TEST_CASE(test_a_failed_open_throws_log_error_and_registers_nothing) {
    // a regular file where the parent directory should be
    std::ofstream blocker(file("blocker"));
    blocker << "not a directory";
    blocker.close();

    BOOST_CHECK_THROW(enable_file(file("blocker") / "x.log"), log_error);
    BOOST_TEST(log_is_init());
    disable_file(file("blocker") / "x.log"); // a no-op: nothing was registered
}

BOOST_AUTO_TEST_CASE(test_an_empty_band_is_refused) {
    BOOST_CHECK_THROW(enable_file(file(), severity_level::error, severity_level::debug), log_error);
    BOOST_TEST(!fs::exists(file()));
}

BOOST_AUTO_TEST_CASE(test_the_pair_is_idempotent) {
    enable_file(file());
    enable_file(file(), severity_level::error); // the second enable on a path does nothing
    sim_estab_log("test.file", severity_level::info, "still the first band");
    disable_file(file());
    disable_file(file());
    disable_file(file("never-enabled.log"));
    BOOST_TEST(contains(read_file(file()), "still the first band"));
    BOOST_TEST(!fs::exists(file("test.01.log")));
}

BOOST_AUTO_TEST_CASE(test_log_deinit_removes_the_files) {
    enable_file(file());
    log_deinit();
    log_init();
    disable_console();
    sim_estab_log("test.file", severity_level::info, "after the reinit");
    BOOST_TEST(!contains(read_file(file()), "after the reinit"));
}

BOOST_AUTO_TEST_CASE(test_a_percent_in_the_name_is_literal) {
    // Boost never sees the name, so a placeholder like %Y stays as it is written
    const fs::path path = file("run_%Y_%%.log");
    enable_file(path);
    sim_estab_log("test.file", severity_level::info, "literal name");
    disable_file(path);
    BOOST_TEST(contains(read_file(path), "literal name"));
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir)) {
        ++count;
    }
    BOOST_TEST(count == 1u); // no expanded name beside it
}

BOOST_AUTO_TEST_CASE(test_two_spellings_name_one_sink) {
    const fs::path plain    = file();
    const fs::path dotted   = dir / "." / "sub" / ".." / "test.log";
    const fs::path relative = fs::relative(plain);
    enable_file(plain);
    enable_file(dotted);   // the same key: a no-op, so no rotation
    enable_file(relative); // the same key again
    sim_estab_log("test.file", severity_level::info, "one sink");
    disable_file(relative); // removes the sink that enable_file(plain) opened
    sim_estab_log("test.file", severity_level::info, "after the disable");

    const std::string text = read_file(plain);
    BOOST_TEST(contains(text, "one sink"));
    BOOST_TEST(!contains(text, "after the disable"));
    BOOST_TEST(!fs::exists(file("test.01.log")));
}

BOOST_AUTO_TEST_CASE(test_the_slot_pair_underneath) {
    static_assert(ConsoleSlot::name == "console");
    static_assert(FileSlot::name == "file");
    static_assert(ConsoleSlot::auto_flush && !FileSlot::auto_flush);
    static_assert(SinkSlot<ConsoleSlot> && SinkSlot<FileSlot>);

    enable<FileSlot>({file(), severity_level::info, severity_level::critical, file_open::truncate});
    sim_estab_log("test.file", severity_level::info, "through the slot");
    disable<FileSlot>(file());
    BOOST_TEST(contains(read_file(file()), "through the slot"));

    CapturedConsole console;
    enable<ConsoleSlot>({});
    sim_estab_log("test.file", severity_level::info, "console through the slot");
    disable<ConsoleSlot>({});
    sim_estab_log("test.file", severity_level::info, "console is off");
    BOOST_TEST(contains(console.text(), "console through the slot"));
    BOOST_TEST(!contains(console.text(), "console is off"));
}

BOOST_AUTO_TEST_CASE(test_the_toggles_are_no_ops_before_init) {
    log_deinit();
    enable_file(file());
    set_severity(severity_level::trace);
    disable_file(file());
    BOOST_TEST(!fs::exists(file()));
    log_init(); // the fixture's teardown expects an initialized subsystem
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_CASE(test_max_log_files) { BOOST_TEST(sim_estab::core::limits::max_log_files == 100u); }

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
