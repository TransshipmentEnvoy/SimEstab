#ifndef LIBSIM_ESTAB__LOG_H
#define LIBSIM_ESTAB__LOG_H

#include <stdexcept>
#include <string_view>

// Boost.Log includes for direct usage in macros
#include <boost/log/attributes.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>

/**
 * @namespace sim_estab::core::log
 * @brief Hierarchical logging system for SimEstab
 *
 * This namespace provides a complete logging solution with the following features:
 * - Hierarchical channel-based organization
 * - Multiple severity levels (trace through critical)
 * - Direct Boost.Log integration without wrapper overhead
 * - Convenient logging macros
 * - No stringstream usage for optimal performance
 *
 * Basic usage:
 * 1. Initialize the logging system: log_init()
 * 2. Use logging macros: SIM_ESTAB_LOG("channel", info, "Message content")
 * 3. Or use convenience macros: SIM_ESTAB_LOG_INFO("channel", "Message content")
 */
namespace sim_estab::core::log {

/**
 * Severity levels for logging
 * Ordered from lowest (trace) to highest (critical) severity
 */
enum class severity_level : int {
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
template <typename CharT> std::basic_ostream<CharT>& operator<<(std::basic_ostream<CharT>& os, severity_level level) {
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
class log_error : public std::runtime_error {
public:
    explicit log_error(const std::string& what) : std::runtime_error(what) {}
};

namespace detail {
/**
 * Type aliases for Boost.Log channel loggers
 *
 * channel_logger: Single-threaded logger for non-concurrent usage
 * channel_logger_mt: Multi-threaded logger for concurrent access
 *
 * These typedefs provide semantic clarity and future-proof the API
 */
using channel_logger    = boost::log::sources::severity_channel_logger<severity_level, std::string>;
using channel_logger_mt = boost::log::sources::severity_channel_logger_mt<severity_level, std::string>;
} // namespace detail

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
void log_init();

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
void log_deinit() noexcept;

/**
 * Check if the logging system is initialized
 * @return true if logging system is initialized and ready to use, false otherwise
 */
bool log_is_init() noexcept;

} // namespace sim_estab::core::log

/**
 * Core logging macro with explicit channel and level
 *
 * This is the fundamental logging macro that all other logging macros build upon.
 * It provides direct channel-based logging using Boost.Log's streaming interface
 * without any stringstream overhead.
 *
 * @param ch Hierarchical channel name (e.g., "sim_estab.network.tcp")
 * @param lvl Severity level (trace, debug, info, warning, error, critical)
 * @param msg Message content - supports stream operators for formatting
 *
 * Usage:
 *   SIM_ESTAB_LOG("my.channel", debug, "Processing file " << filename << " with " << count << " items");
 */
#define SIM_ESTAB_LOG(ch, lvl, msg)                                                                                    \
    do {                                                                                                               \
        if (::sim_estab::core::log::log_is_init()) {                                                                   \
            ::sim_estab::core::log::detail::channel_logger_mt logger(::boost::log::keywords::channel = (ch));          \
            BOOST_LOG_SEV(logger, ::sim_estab::core::log::severity_level::lvl) << msg;                                 \
        }                                                                                                              \
    } while (0)

/**
 * Convenience macros for direct channel-based logging
 *
 * These macros provide a simplified interface for logging messages at specific
 * severity levels using direct channel names.
 */

/**
 * Log a trace-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_TRACE("sim_estab.network", "Connection established to " << host);
 */
#define SIM_ESTAB_LOG_TRACE(ch, msg) SIM_ESTAB_LOG(ch, trace, msg)

/**
 * Log a debug-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_DEBUG("sim_estab.physics", "Updating physics step " << step_count);
 */
#define SIM_ESTAB_LOG_DEBUG(ch, msg) SIM_ESTAB_LOG(ch, debug, msg)

/**
 * Log an info-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_INFO("sim_estab.app", "Application started successfully");
 */
#define SIM_ESTAB_LOG_INFO(ch, msg) SIM_ESTAB_LOG(ch, info, msg)

/**
 * Log a warning-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_WARN("sim_estab.render", "Low memory warning: " << available_mb << " MB remaining");
 */
#define SIM_ESTAB_LOG_WARN(ch, msg) SIM_ESTAB_LOG(ch, warning, msg)

/**
 * Log an error-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_ERROR("sim_estab.io", "Failed to load file: " << filename << " - " << error_msg);
 */
#define SIM_ESTAB_LOG_ERROR(ch, msg) SIM_ESTAB_LOG(ch, error, msg)

/**
 * Log a critical-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_CRITICAL("sim_estab.core", "Fatal error: " << error_description);
 */
#define SIM_ESTAB_LOG_CRITICAL(ch, msg) SIM_ESTAB_LOG(ch, critical, msg)

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

#endif