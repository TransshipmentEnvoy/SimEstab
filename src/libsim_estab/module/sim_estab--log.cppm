/**
 * @file sim_estab--log.cppm
 * @brief Logging module interface for SimEstab
 *
 * This module provides the logging subsystem API as a C++20 module interface.
 * It exports all public functions and types while keeping macro definitions
 * in the traditional header file (log.h) for consumer use.
 *
 * Design notes:
 * - Macros (SIM_ESTAB_LOG_*, etc.) remain in log.h and must be #include'd
 * - Boost.Log dependencies are kept in implementation files only
 * - All exported entities use modern C++ features
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <functional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

// Boost
#include <boost/log/core.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/keywords/severity.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

// Module declaration
export module sim_estab:log;

// Note: We don't import std module as GCC's std module support is still experimental
// Instead, we rely on headers included in the global module fragment above

/**
 * @namespace sim_estab::core::log
 * @brief Hierarchical logging system for SimEstab
 *
 * This namespace provides a complete logging solution with the following features:
 * - Hierarchical channel-based organization
 * - Multiple severity levels (trace through critical)
 * - Direct Boost.Log integration without wrapper overhead
 * - Convenient logging macros (in log.h)
 * - No stringstream usage for optimal performance
 *
 * Basic usage:
 * 1. Import the module: import sim_estab:log;
 * 2. Include the macro header: #include <sim_estab/log.h>
 * 3. Initialize the logging system: log_init()
 * 4. Use logging macros: SIM_ESTAB_LOG("channel", info, "Message content")
 */
namespace sim_estab::core::log {

/**
 * Severity levels for logging
 * Ordered from lowest (trace) to highest (critical) severity
 */
export enum class severity_level : int {
    trace    = 0,  ///< Fine-grained tracing messages (lowest)
    debug    = 10, ///< Debug information for development
    info     = 20, ///< General operational information
    warning  = 30, ///< Warning messages for non-critical issues
    error    = 40, ///< Error messages for recoverable problems
    critical = 50  ///< Critical/Fatal error messages (highest, irrecoverable)
};

/**
 * Stream output operator for severity levels
 *
 * Converts severity level enumeration values to human-readable string representations
 * for use in log formatting and output streams.
 *
 * @param os Output stream to write to
 * @param level Severity level to convert to string
 * @return Reference to the output stream for chaining
 */
export template <typename CharT>
std::basic_ostream<CharT>& operator<<(std::basic_ostream<CharT>& os, severity_level level) {
    switch (level) {
    case severity_level::trace:
        return os << "TRACE";
    case severity_level::debug:
        return os << "DEBUG";
    case severity_level::info:
        return os << "INFO";
    case severity_level::warning:
        return os << "WARNING";
    case severity_level::error:
        return os << "ERROR";
    case severity_level::critical:
        return os << "CRITICAL";
    default:
        return os << "UNKNOWN";
    }
}

/**
 * Exception class for logging-related errors
 *
 * Thrown when logging operations fail, such as attempting to use
 * the logging system before initialization or when internal logging
 * errors occur.
 */
export class log_error : public std::runtime_error {
public:
    explicit log_error(const std::string& what) : std::runtime_error(what) {}
};

/**
 * Initialize the hierarchical logging system
 *
 * Sets up the complete logging infrastructure with the following components:
 * - Console sink writing to std::clog with automatic flushing
 * - Timestamp formatting with microsecond precision
 * - Severity level display
 * - Hierarchical channel name display
 * - Default severity filter set to INFO level and above
 * - Common Boost.Log attributes (timestamp, thread ID, etc.)
 *
 * This function is thread-safe and idempotent - multiple calls are safe
 * and subsequent calls after the first will be ignored.
 *
 * @note Must be called before any logging operations
 * @throws May throw boost::log exceptions if system resources are insufficient
 */
export void log_init();

/**
 * Deinitialize the logging system
 *
 * Performs complete cleanup of the logging system in the following order:
 * 1. Flushes all pending log records to ensure no data loss
 * 2. Removes all configured sinks
 * 3. Resets initialization status to allow re-initialization
 *
 * This function is thread-safe, idempotent, and exception-safe.
 * Multiple calls are safe - subsequent calls after the first will be ignored.
 *
 * @note After calling this function, log_init() must be called again
 *       before any logging operations can be performed.
 */
export void log_deinit() noexcept;

/**
 * Check if the logging system is initialized
 * @return true if logging system is initialized and ready to use, false otherwise
 */
export bool log_is_init() noexcept;

/**
 * Enable console logging output
 *
 * This function adds a console sink to the logging system if one doesn't already exist.
 * The console sink will output colored log messages to std::clog with automatic flushing.
 *
 * This function is thread-safe and idempotent - multiple calls are safe
 * and subsequent calls after the first will be ignored.
 *
 * @note The logging system must be initialized before calling this function
 * @throws May throw boost::log exceptions if system resources are insufficient
 */
export void enable_console();

/**
 * Disable console logging output
 *
 * This function removes the console sink from the logging system if it exists.
 * All pending log records will be flushed before removal.
 *
 * This function is thread-safe and idempotent - multiple calls are safe
 * and subsequent calls after the first will be ignored.
 *
 * @note The logging system must be initialized before calling this function
 */
export void disable_console() noexcept;

// concept: "printable to std::ostream"
template <class T>
concept OStreamable = requires(std::ostream& os, T&& value) {
    { os << std::forward<T>(value) } -> std::same_as<std::ostream&>;
};

// Forward declare implementation. Note: not exported.
namespace detail {
void sim_estab_log_impl(const std::string& channel, severity_level lvl,
                        std::function<void(boost::log::record_ostream&)> msg_fn);
} // namespace detail

// abbreviated function template
//
// - `auto`/`OStreamable auto` are placeholder types (abbreviated templates)
// - export makes it usable from other translation units via `import sim_estab.log;`
export void sim_estab_log(const std::string& ch,               // channel (string, string_view, etc.)
                          severity_level sev,              // severity type (your enum, etc.)
                          OStreamable auto&&...msg // parts of the message, all OStreamable
) {
    if (!log_is_init()) {
        return;
    }

    /*
    detail::channel_logger_mt logger(boost::log::keywords::channel = std::forward<decltype(ch)>(ch));

    // Use the Boost.Log macro inside the module
    using keyword::severity;
    auto& rec = logger.open_record(severity = std::forward<decltype(sev)>(sev));
    if (!rec)
        return;

    boost::log::record_ostream rec_stream(rec);

    // Fold-expression over the variadic message parts:
    // rec_stream << part1 << part2 << ...
    (rec_stream << ... << std::forward<decltype(msg)>(msg));

    rec_stream.flush();
    logger.push_record(boost::move(rec));
    */

    // workaround
    auto msg_fn = [... msg_pack = std::forward<decltype(msg)>(msg)](boost::log::record_ostream& rec_stream) mutable {
        (rec_stream << ... << std::forward<decltype(msg_pack)>(msg_pack));
    };
    detail::sim_estab_log_impl(std::forward<decltype(ch)>(ch), std::forward<decltype(sev)>(sev), std::move(msg_fn));
}

} // namespace sim_estab::core::log

/**
 * @brief Channel Naming Convention
 *
 * Use hierarchical channel names following the pattern: "module.subsystem.component"
 *
 * Examples:
 * - "sim_estab.network.tcp"     - TCP networking layer
 * - "sim_estab.render.opengl"   - OpenGL rendering
 * - "sim_estab.physics.bullet"  - Bullet physics integration
 * - "sim_estab.audio.openal"    - OpenAL audio subsystems
 * - "sim_estab.ui.imgui"        - ImGui user interface
 *
 * This enables runtime filtering by patterns like:
 * - "sim_estab.*" - All SimEstab channels
 * - "sim_estab.network.*" - Only networking channels
 * - "*.physics.*" - All physics-related channels
 */
