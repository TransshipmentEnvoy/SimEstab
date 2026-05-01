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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

// ============================================================================
// Opaque wrapper classes for Boost.Log types
// These classes use fast pimpl to completely hide Boost.Log from the interface
// ============================================================================

/**
 * @brief Opaque wrapper for boost::log::record
 *
 * Represents a log record that holds attribute values. The record can be
 * checked for validity using boolean conversion operators.
 *
 * Move-only type matching Boost.Log's record semantics.
 */
export class record {
public:
    /// Default constructor creates an invalid record
    record() noexcept;

    /// Move constructor
    record(record&& other) noexcept;

    /// Destructor
    ~record() noexcept;

    /// Move assignment operator
    record& operator=(record&& other) noexcept;

    /// Non-copyable
    record(const record&)            = delete;
    record& operator=(const record&) = delete;

    /// Check if record is valid (explicit bool conversion)
    explicit operator bool() const noexcept;

    /// Check if record is invalid
    bool operator!() const noexcept;

    /// Swap with another record
    void swap(record& other) noexcept;

    /// Reset to invalid state, releasing resources
    void reset() noexcept;

    // Fast pimpl storage size/alignment constants (public for static_assert in impl)
    static constexpr std::size_t storage_size  = 16;
    static constexpr std::size_t storage_align = 8;

private:
    friend class record_ostream;
    friend class logger;
    friend class logger_mt;

    alignas(storage_align) std::byte storage_[storage_size];

    // Access internal implementation
    void *get_impl() noexcept;
    const void *get_impl() const noexcept;
};

/// Free-standing swap for record
export inline void swap(record& lhs, record& rhs) noexcept { lhs.swap(rhs); }

/**
 * @brief Opaque wrapper for boost::log::record_ostream
 *
 * Provides streaming capability for composing log messages.
 * Wraps a record and provides operator<< for various types.
 *
 * Non-copyable, non-movable (matches Boost.Log semantics).
 */
export class record_ostream {
public:
    /// Default constructor creates detached stream
    record_ostream() noexcept;

    /// Constructor attaching to a record
    /// @pre rec must be valid (!!rec == true)
    explicit record_ostream(record& rec);

    /// Destructor - detaches from record
    ~record_ostream() noexcept;

    /// Non-copyable, non-movable
    record_ostream(const record_ostream&)            = delete;
    record_ostream& operator=(const record_ostream&) = delete;
    record_ostream(record_ostream&&)                 = delete;
    record_ostream& operator=(record_ostream&&)      = delete;

    /// Check if stream is valid and ready for formatting
    explicit operator bool() const noexcept;

    /// Check if stream is invalid
    bool operator!() const noexcept;

    /// Get the attached record
    /// @pre Stream must be attached to a record
    record& get_record();
    const record& get_record() const;

    /// Attach to a new record (detaches from current if any)
    /// @pre rec must be valid
    void attach_record(record& rec);

    /// Detach from current record
    void detach_from_record() noexcept;

    /// Flush the stream
    record_ostream& flush();

    // ========================================================================
    // Explicit operator<< overloads for basic types
    // These forward directly to boost::log::record_ostream without overhead
    // ========================================================================

    record_ostream& operator<<(bool value);
    record_ostream& operator<<(char value);
    record_ostream& operator<<(signed char value);
    record_ostream& operator<<(unsigned char value);
    record_ostream& operator<<(short value);
    record_ostream& operator<<(unsigned short value);
    record_ostream& operator<<(int value);
    record_ostream& operator<<(unsigned int value);
    record_ostream& operator<<(long value);
    record_ostream& operator<<(unsigned long value);
    record_ostream& operator<<(long long value);
    record_ostream& operator<<(unsigned long long value);
    record_ostream& operator<<(float value);
    record_ostream& operator<<(double value);
    record_ostream& operator<<(long double value);
    record_ostream& operator<<(const char *value);
    record_ostream& operator<<(const wchar_t *value);
    record_ostream& operator<<(const void *value);
    record_ostream& operator<<(const std::string& value);
    record_ostream& operator<<(std::string_view value);

    // IO manipulators
    record_ostream& operator<<(std::ostream& (*manip)(std::ostream&));
    record_ostream& operator<<(std::ios_base& (*manip)(std::ios_base&));

    /**
     * @brief Template fallback for custom types
     *
     * Uses std::ostringstream to convert to string, then writes.
     * This has some overhead but allows any OStreamable type to work.
     *
     * ## How to provide custom formatting:
     *
     * Users can define a non-template free function in sim_estab::core::log namespace
     * that will be found via ADL and take precedence over this member template:
     *
     * @code
     * namespace sim_estab::core::log {
     *     // Non-template overload - higher priority than member template
     *     record_ostream& operator<<(record_ostream& os, const MyType& t) {
     *         return os << "MyType: " << t.value();
     *     }
     * }
     * @endcode
     *
     * @note This member template is a fallback; non-template free functions
     *       in the sim_estab::core::log namespace take precedence.
     */
    template <typename T>
        requires OStreamable<T> && (!std::is_same_v<std::decay_t<T>, bool>) &&
                 (!std::is_same_v<std::decay_t<T>, char>) && (!std::is_same_v<std::decay_t<T>, signed char>) &&
                 (!std::is_same_v<std::decay_t<T>, unsigned char>) && (!std::is_same_v<std::decay_t<T>, short>) &&
                 (!std::is_same_v<std::decay_t<T>, unsigned short>) && (!std::is_same_v<std::decay_t<T>, int>) &&
                 (!std::is_same_v<std::decay_t<T>, unsigned int>) && (!std::is_same_v<std::decay_t<T>, long>) &&
                 (!std::is_same_v<std::decay_t<T>, unsigned long>) && (!std::is_same_v<std::decay_t<T>, long long>) &&
                 (!std::is_same_v<std::decay_t<T>, unsigned long long>) && (!std::is_same_v<std::decay_t<T>, float>) &&
                 (!std::is_same_v<std::decay_t<T>, double>) && (!std::is_same_v<std::decay_t<T>, long double>) &&
                 (!std::is_same_v<std::decay_t<T>, const char *>) && (!std::is_same_v<std::decay_t<T>, char *>) &&
                 (!std::is_same_v<std::decay_t<T>, const wchar_t *>) && (!std::is_same_v<std::decay_t<T>, wchar_t *>) &&
                 (!std::is_same_v<std::decay_t<T>, const void *>) && (!std::is_same_v<std::decay_t<T>, void *>) &&
                 (!std::is_same_v<std::decay_t<T>, std::string>) && (!std::is_same_v<std::decay_t<T>, std::string_view>)
    record_ostream& operator<<(T&& value) {
        std::ostringstream oss;
        oss << std::forward<T>(value);
        return *this << oss.str();
    }

    // Fast pimpl storage size/alignment constants (public for static_assert in impl)
    static constexpr std::size_t storage_size  = 512;
    static constexpr std::size_t storage_align = 16;

private:
    alignas(storage_align) std::byte storage_[storage_size];
    record *attached_record_{nullptr};

    void *get_impl() noexcept;
    const void *get_impl() const noexcept;
};

/**
 * @brief Opaque wrapper for boost::log::sources::severity_channel_logger
 *
 * Single-threaded logger with severity level and channel support.
 * Use logger_mt for multi-threaded scenarios.
 */
export class logger {
public:
    /// Construct logger with channel name
    explicit logger(const std::string& channel);

    /// Copy constructor
    logger(const logger& other);

    /// Move constructor
    logger(logger&& other) noexcept;

    /// Destructor
    ~logger() noexcept;

    /// Copy assignment
    logger& operator=(const logger& other);

    /// Move assignment
    logger& operator=(logger&& other) noexcept;

    /// Swap with another logger
    void swap(logger& other) noexcept;

    /// Get the channel name
    std::string channel() const;

    /// Open a new log record with specified severity
    /// @return Valid record if logging is enabled and passes filters, invalid otherwise
    record open_record(severity_level level);

    /// Push a completed record to sinks
    /// @param rec Record to push (will be moved from)
    void push_record(record&& rec);

    // Fast pimpl storage size/alignment constants (public for static_assert in impl)
    static constexpr std::size_t storage_size  = 256;
    static constexpr std::size_t storage_align = 16;

private:
    alignas(storage_align) std::byte storage_[storage_size];

    void *get_impl() noexcept;
    const void *get_impl() const noexcept;
};

/// Free-standing swap for logger
export inline void swap(logger& lhs, logger& rhs) noexcept { lhs.swap(rhs); }

/**
 * @brief Opaque wrapper for boost::log::sources::severity_channel_logger_mt
 *
 * Multi-threaded logger with severity level and channel support.
 * Thread-safe for concurrent logging operations.
 */
export class logger_mt {
public:
    /// Construct logger with channel name
    explicit logger_mt(const std::string& channel);

    /// Copy constructor
    logger_mt(const logger_mt& other);

    /// Move constructor
    logger_mt(logger_mt&& other) noexcept;

    /// Destructor
    ~logger_mt() noexcept;

    /// Copy assignment
    logger_mt& operator=(const logger_mt& other);

    /// Move assignment
    logger_mt& operator=(logger_mt&& other) noexcept;

    /// Swap with another logger
    void swap(logger_mt& other) noexcept;

    /// Get the channel name
    std::string channel() const;

    /// Open a new log record with specified severity
    /// @return Valid record if logging is enabled and passes filters, invalid otherwise
    record open_record(severity_level level);

    /// Push a completed record to sinks
    /// @param rec Record to push (will be moved from)
    void push_record(record&& rec);

    // Fast pimpl storage size/alignment constants (public for static_assert in impl)
    static constexpr std::size_t storage_size  = 512;
    static constexpr std::size_t storage_align = 16;

private:
    alignas(storage_align) std::byte storage_[storage_size];

    void *get_impl() noexcept;
    const void *get_impl() const noexcept;
};

/// Free-standing swap for logger_mt
export inline void swap(logger_mt& lhs, logger_mt& rhs) noexcept { lhs.swap(rhs); }

// abbreviated function template
//
// - `auto`/`OStreamable auto` are placeholder types (abbreviated templates)
// - export makes it usable from other translation units via `import sim_estab.log;`
export void sim_estab_log(const std::string& ch,   // channel (string, string_view, etc.)
                          severity_level sev,      // severity type (your enum, etc.)
                          OStreamable auto&&...msg // parts of the message, all OStreamable
) {
    if (!log_is_init()) {
        return;
    }

    // Directly use the exported opaque wrapper classes
    logger_mt lg(ch);
    record rec = lg.open_record(sev);
    if (!rec) {
        return;
    }

    record_ostream strm(rec);
    (strm << ... << std::forward<decltype(msg)>(msg));
    strm.flush();
    lg.push_record(std::move(rec));
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
