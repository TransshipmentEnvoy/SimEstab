/**
 * @file sim_estab--log.cppm
 * @brief Logging module interface for SimEstab
 *
 * This module provides the logging subsystem API as a C++20 module interface.
 * It exports all public functions and types while keeping macro definitions
 * in the traditional header file (macro.h) for consumer use.
 *
 * Design notes:
 * - Macros (SIM_ESTAB_LOG_*, etc.) remain in macro.h and must be #include'd
 * - Boost.Log dependencies are kept in implementation files only
 * - All exported entities use modern C++ features
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <ios>
#include <meta>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Module declaration
export module sim_estab:log;

import :error;

// Note: We don't import std module as GCC's std module support is still experimental
// Instead, we rely on headers included in the global module fragment above

/**
 * @namespace sim_estab::core::log
 * @brief Hierarchical logging system for SimEstab
 *
 * This namespace provides a complete logging solution with the following features:
 * - Hierarchical channel-based organization
 * - Multiple severity levels (trace through critical)
 * - Boost.Log hidden behind opaque fast-pimpl wrappers (design_patterns.md §5b)
 * - Convenient logging macros (in macro.h)
 * - No stringstream usage for optimal performance
 *
 * Basic usage:
 * 1. Import the module: import sim_estab;
 * 2. Include the macro header: #include <sim_estab/macro.h>
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
 * What enable_file does to a file that exists (design_logging.md §3 "Open")
 */
export enum class file_open : int {
    rotate   = 0, ///< the file becomes the newest backup, and the active file starts empty
    append   = 1, ///< the file continues; the backups are not touched
    truncate = 2, ///< the file starts over; the backups are not touched
};

// ============================================================================
// Names from the enums (design_logging.md §3 "Names from the enums")
//
// C++26 reflection lists the enumerators of severity_level and file_open once. The name
// of a value, the parse of a name, the console line's upper-case severity and the name
// lists that _if.log exports all come from that one table per enum, so each name is
// written once, in the enum.
// ============================================================================

/// The two enums whose names are generated
export template <class E>
concept LogEnum = std::same_as<E, severity_level> || std::same_as<E, file_open>;

namespace detail {

/// The enumerators of E as (value, identifier) pairs, in declaration order, with the
/// identifiers in static storage
template <LogEnum E> consteval auto make_enum_table() {
    constexpr auto count = std::meta::enumerators_of(^^E).size();
    std::array<std::pair<E, std::string_view>, count> table{};
    std::size_t i = 0;
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
        table[i++] = std::pair<E, std::string_view>{[:e:], std::define_static_string(std::meta::identifier_of(e))};
    }
    return table;
}

/// `name` in upper case, in static storage: the console line and the file line print
/// the severity this way
consteval std::string_view upper(std::string_view name) {
    std::vector<char> out(name.begin(), name.end());
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return std::string_view{std::define_static_string(out)};
}

template <LogEnum E> consteval auto make_upper_table() {
    constexpr auto table = make_enum_table<E>();
    std::array<std::pair<E, std::string_view>, table.size()> upper_table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        upper_table[i] = {table[i].first, upper(table[i].second)};
    }
    return upper_table;
}

template <LogEnum E> inline constexpr auto enum_table  = make_enum_table<E>();
template <LogEnum E> inline constexpr auto upper_table = make_upper_table<E>();

} // namespace detail

/// The severity names, lowest first: ("trace", "debug", ...). Bound as
/// _if.log.severity_level_names; config.py reads LOG_LEVELS from it.
export inline constexpr auto severity_level_names = detail::enum_table<severity_level>;

/// The open modes, in declaration order: ("rotate", "append", "truncate")
export inline constexpr auto file_open_names = detail::enum_table<file_open>;

/// The identifier of a value, "unknown" for a value outside the enum
export template <LogEnum E> [[nodiscard]] constexpr std::string_view name_of(E value) noexcept {
    for (const auto& [v, name] : detail::enum_table<E>) {
        if (v == value) {
            return name;
        }
    }
    return "unknown";
}

/// The value of an identifier, nullopt for a name outside the enum
export template <LogEnum E> [[nodiscard]] constexpr std::optional<E> from_name(std::string_view name) noexcept {
    for (const auto& [v, n] : detail::enum_table<E>) {
        if (n == name) {
            return v;
        }
    }
    return std::nullopt;
}

/**
 * Stream output operator for severity levels: the upper-case name, as the log line
 * prints it ("INFO")
 */
export template <typename CharT>
std::basic_ostream<CharT>& operator<<(std::basic_ostream<CharT>& os, severity_level level) {
    for (const auto& [v, name] : detail::upper_table<severity_level>) {
        if (v == level) {
            return os << name;
        }
    }
    return os << "UNKNOWN";
}

/// Stream output operator for open modes: the identifier ("rotate")
export template <typename CharT> std::basic_ostream<CharT>& operator<<(std::basic_ostream<CharT>& os, file_open mode) {
    return os << name_of(mode);
}

/**
 * Exception class for logging-related errors
 *
 * Thrown when logging operations fail, such as attempting to use
 * the logging system before initialization or when internal logging
 * errors occur.
 */
export class log_error : public error::sim_estab_error {
public:
    explicit log_error(const std::string& what) : error::sim_estab_error(what) {}
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
export [[nodiscard]] bool log_is_init() noexcept;

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

// ============================================================================
// File sinks and the severity filter (design_logging.md §3)
// ============================================================================

/**
 * Open a file sink on `path` (design_logging.md §3 "One toggle pair, many files")
 *
 * The file receives a record when `floor <= severity <= ceiling`. `on_open` says what
 * happens to a file that exists; under `rotate` the file becomes backup `.01`, each
 * backup moves up by one, and the one past limits::max_log_files is deleted (§3
 * "Rotation"). The file opens at once, and a failed open throws log_error naming the
 * path. Right after the open, the file header goes out as ordinary `info` records (§3
 * "The file header").
 *
 * The sink is keyed by the absolute, lexically normal path, so two spellings of one path
 * name one sink; disable_file normalizes its path the same way. Symlinks are not resolved.
 *
 * Toggle shape (design_patterns.md §2): a no-op when the subsystem is not initialized,
 * a no-op on a path that is already enabled, safe from any thread. To change a file's
 * band or open mode, disable it and enable it again.
 *
 * @throws log_error when the open or the rotation fails, or when the band is empty
 */
export void enable_file(const std::filesystem::path& path, severity_level floor = severity_level::trace,
                        severity_level ceiling = severity_level::critical, file_open on_open = file_open::rotate);

/**
 * Remove the file sink on `path`, if there is one. Pending records are flushed first.
 * The path is normalized as enable_file normalizes it.
 */
export void disable_file(const std::filesystem::path& path) noexcept;

/**
 * Set the user's level (design_logging.md §3 "The core filter and set_severity")
 *
 * The console carries the user's level as its own filter. The core filter is the minimum
 * of the user's level and the floors of the enabled file sinks. A file's band is fixed at
 * its open: this call never moves a band. A no-op when the subsystem is not initialized;
 * log_init resets the level to `info`.
 */
export void set_severity(severity_level level);

// ============================================================================
// Sink slots (design_logging.md §3 "Sink slots underneath the toggles")
//
// Each sink kind is a type that carries its fixed policy as constexpr members. One pair
// of function templates, enable<Slot> and disable<Slot>, implements every named pair
// above, and log.cpp maps each slot to its Boost formatter, so Boost stays out of this
// interface. The slot types and the pair are C++ interface: engine code may use them
// directly. They are not bound to Python.
// ============================================================================

/// The console slot: one sink on std::clog, coloured, flushed on every record
export struct ConsoleSlot {
    static constexpr std::string_view name = "console";
    static constexpr bool auto_flush       = true; ///< a terminal shows a record when it is written

    struct params {}; ///< the console takes no parameters
    struct key {};    ///< and holds one sink
};

/// The file slot: one sink per path, plain lines, buffered
export struct FileSlot {
    static constexpr std::string_view name = "file";
    static constexpr bool auto_flush       = false; ///< buffered; a record of error or above flushes every sink

    struct params {
        std::filesystem::path path;
        severity_level floor   = severity_level::trace;
        severity_level ceiling = severity_level::critical;
        file_open on_open      = file_open::rotate;
    };
    using key = std::filesystem::path;
};

/// A sink slot: a type with the two policy members and the two nested types above
export template <class Slot>
concept SinkSlot = requires {
    { Slot::name } -> std::convertible_to<std::string_view>;
    { Slot::auto_flush } -> std::convertible_to<bool>;
    typename Slot::params;
    typename Slot::key;
};

/// Enable one sink of `Slot`. enable_console() is enable<ConsoleSlot>({}), and
/// enable_file(...) is enable<FileSlot>({...}). Defined in log.cpp for each slot.
export template <SinkSlot Slot> void enable(const typename Slot::params& params);

/// Disable the sink of `Slot` that `key` names. disable_console() is
/// disable<ConsoleSlot>({}), and disable_file(path) is disable<FileSlot>(path).
export template <SinkSlot Slot> void disable(const typename Slot::key& key) noexcept;

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
    record_ostream& operator<<(decltype(std::setprecision(0)) manip);
    record_ostream& operator<<(decltype(std::setw(0)) manip);
    record_ostream& operator<<(decltype(std::setfill(' ')) manip);
    record_ostream& operator<<(decltype(std::setbase(10)) manip);
    record_ostream& operator<<(decltype(std::setiosflags(std::ios_base::fmtflags{})) manip);
    record_ostream& operator<<(decltype(std::resetiosflags(std::ios_base::fmtflags{})) manip);
    record_ostream& operator<<(decltype(std::put_money(std::declval<long double>())) manip);
    record_ostream& operator<<(decltype(std::put_money(std::declval<const std::string&>())) manip);
    record_ostream& operator<<(decltype(std::put_time(static_cast<const std::tm *>(nullptr), "")) manip);
    record_ostream& operator<<(decltype(std::quoted("")) manip);
    record_ostream& operator<<(decltype(std::quoted(std::declval<const std::string&>())) manip);
    record_ostream& operator<<(decltype(std::quoted(std::declval<std::string&>())) manip);
    record_ostream& operator<<(decltype(std::quoted(std::declval<std::string_view>())) manip);

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

// concept: "printable directly to record_ostream"
export template <class T>
concept LogStreamable = requires(record_ostream& os, T&& value) {
    { os << std::forward<T>(value) } -> std::same_as<record_ostream&>;
};

/**
 * @brief Explicit adapter for types that already support std::ostream output.
 *
 * This is intentionally opt-in. Unknown types are not implicitly stringified by
 * record_ostream; users can call this helper from a record_ostream overload when
 * local stringification is acceptable.
 */
export template <typename T> record_ostream& stream_via_ostream(record_ostream& os, const T& value) {
    std::ostringstream oss;
    oss << value;
    return os << oss.str();
}

// abbreviated function template
//
// - `auto`/`LogStreamable auto` are placeholder types (abbreviated templates)
// - export makes it usable from other translation units via `import sim_estab;`
export void sim_estab_log(const std::string& ch,     // channel (string, string_view, etc.)
                          severity_level sev,        // severity type (your enum, etc.)
                          LogStreamable auto&&...msg // parts of the message, all LogStreamable
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
