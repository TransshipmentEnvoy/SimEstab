/**
 * @file log.cpp
 * @brief Implementation of the SimEstab logging system using C++20 modules
 *
 * This file provides the implementation for functions declared in the
 * sim_estab:log module partition interface.
 *
 * Key design:
 * - This is a module implementation unit for the sim_estab:log partition
 * - It belongs to the sim_estab module's log partition
 * - Boost.Log headers with macros are included in the global module fragment
 * - Implementation details are module-internal
 */

// Global module fragment - for headers with macros and legacy code
module;

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
// #include <shared_mutex>
#include <functional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/container/flat_map.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include <boost/core/null_deleter.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/setup.hpp>

#include <rang.hpp>

// Module declaration - this is the sim_estab implementation unit for the log partition
module sim_estab;

namespace sim_estab::core::log {

namespace {
namespace logging  = boost::log;
namespace sinks    = boost::log::sinks;
namespace expr     = boost::log::expressions;
namespace attrs    = boost::log::attributes;
namespace keywords = boost::log::keywords;
} // namespace

// typedef
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

// color sink related
namespace detail {

/**
 * Apply severity-based colors to output stream
 *
 * Maps severity levels to rang colors and styles:
 * - TRACE: gray + dim
 * - DEBUG: blue
 * - INFO: no color (default terminal color)
 * - WARNING: yellow
 * - ERROR: red
 * - CRITICAL: red + bold + black background
 *
 * @param strm Output stream to apply colors to
 * @param level Severity level to determine color
 */
template <typename StreamT> void apply_severity_colors(StreamT& strm, severity_level level) {
    switch (level) {
    case severity_level::trace:
        strm << rang::fg::gray << rang::style::dim;
        break;
    case severity_level::debug:
        strm << rang::fg::blue;
        break;
    case severity_level::info:
        // No color for INFO level - keep default terminal appearance
        break;
    case severity_level::warning:
        strm << rang::fg::yellow;
        break;
    case severity_level::error:
        strm << rang::fg::red;
        break;
    case severity_level::critical:
        strm << rang::fg::red << rang::style::bold << rang::bg::black;
        break;
    }
}

/**
 * Reset terminal colors to default
 *
 * @param strm Output stream to reset colors on
 */
template <typename StreamT> void reset_colors(StreamT& strm) { strm << rang::style::reset; }

/**
 * Colored text output stream backend for Boost.Log
 *
 * This backend extends the standard text_ostream_backend to add severity-based
 * color coding to log output. It applies colors before the formatted message
 * and resets them after, while preserving all formatting capabilities of the
 * base backend.
 *
 * Features:
 * - Severity-based color coding using rang library
 * - INFO level logs remain uncolored (as requested)
 * - Preserves all expr::format functionality
 * - Thread-safe operation
 * - Compatible with existing Boost.Log infrastructure
 */
template <typename CharT>
class colored_text_ostream_backend
    : public boost::log::sinks::basic_formatted_sink_backend<
          CharT, boost::log::sinks::combine_requirements<boost::log::sinks::synchronized_feeding,
                                                         boost::log::sinks::flushing>::type> {

    // Local using declarations for commonly used boost::log types
    using auto_newline_mode = boost::log::sinks::auto_newline_mode;
    using record_view       = boost::log::record_view;

    //! Base type
    typedef typename boost::log::sinks::basic_formatted_sink_backend<
        CharT, boost::log::sinks::combine_requirements<boost::log::sinks::synchronized_feeding,
                                                       boost::log::sinks::flushing>::type>
        base_type;

public:
    //! Character type
    typedef typename base_type::char_type char_type;
    //! String type to be used as a message text holder
    typedef typename base_type::string_type string_type;
    //! Output stream type
    typedef std::basic_ostream<char_type> stream_type;

private:
    //! Type of the container that holds all aggregated streams
    typedef std::vector<boost::shared_ptr<stream_type>> ostream_sequence;

    //! Output stream list
    ostream_sequence m_streams;
    //! Indicates whether to append a trailing newline after every log record
    auto_newline_mode m_auto_newline_mode;
    //! Auto-flush flag
    bool m_auto_flush;

public:
    /*!
     * Constructor. No streams attached to the constructed backend, auto flush feature disabled.
     */
    colored_text_ostream_backend() : m_auto_newline_mode(auto_newline_mode::insert_if_missing), m_auto_flush(false) {}

    /*!
     * Constructor. Creates a sink backend with the specified named parameters.
     * The following named parameters are supported:
     *
     * \li \c auto_flush - Specifies a flag, whether or not to automatically flush the attached streams after each
     *                     written log record. By default, is \c false.
     * \li \c auto_newline_mode - Specifies automatic trailing newline insertion mode. Must be a value of
     *                            the \c auto_newline_mode enum. By default, is
     * <tt>auto_newline_mode::insert_if_missing</tt>.
     */
    BOOST_LOG_PARAMETRIZED_CONSTRUCTORS_CALL(colored_text_ostream_backend, construct)

    /*!
     * Destructor
     */
    ~colored_text_ostream_backend() = default;

    /*!
     * The method adds a new stream to the sink.
     *
     * \param strm Pointer to the stream. Must not be NULL.
     */
    void add_stream(boost::shared_ptr<stream_type> const& strm) {
        auto it = std::find(m_streams.begin(), m_streams.end(), strm);
        if (it == m_streams.end()) {
            m_streams.push_back(strm);
        }
    }

    /*!
     * The method removes a stream from the sink. If the stream is not attached to the sink,
     * the method has no effect.
     *
     * \param strm Pointer to the stream. Must not be NULL.
     */
    void remove_stream(boost::shared_ptr<stream_type> const& strm) {
        auto it = std::find(m_streams.begin(), m_streams.end(), strm);
        if (it != m_streams.end())
            m_streams.erase(it);
    }

    /*!
     * Sets the flag to automatically flush buffers of all attached streams after each log record.
     *
     * \param enable The flag indicates whether the automatic buffer flush should be performed.
     */
    void auto_flush(bool enable = true) { m_auto_flush = enable; }

    /*!
     * Selects whether a trailing newline should be automatically inserted after every log record. See
     * \c auto_newline_mode description for the possible modes of operation.
     *
     * \param mode The trailing newline insertion mode.
     */
    void set_auto_newline_mode(auto_newline_mode mode) { m_auto_newline_mode = mode; }

    /*!
     * The method writes the message to the sink.
     */
    void consume(record_view const& rec, string_type const& formatted_message) {
        // Extract severity level from the record for color application
        auto severity_attr   = rec[boost::log::aux::default_attribute_names::severity()];
        severity_level level = severity_level::info; // default fallback

        if (severity_attr) {
            level = severity_attr.extract<severity_level>().get();
        }

        typename string_type::const_pointer const p = formatted_message.data();
        typename string_type::size_type const s     = formatted_message.size();

        bool need_trailing_newline = false;
        if (m_auto_newline_mode != auto_newline_mode::disabled_auto_newline) {
            need_trailing_newline = (m_auto_newline_mode == auto_newline_mode::always_insert || s == 0u ||
                                     p[s - 1u] != static_cast<char_type>('\n'));
        }

        for (auto const& stream_ptr : m_streams) {
            stream_type *const strm = stream_ptr.get();
            if (strm->good()) [[likely]] {
                // Apply severity-based colors
                apply_severity_colors(*strm, level);

                // Write the formatted message
                strm->write(p, static_cast<std::streamsize>(s));

                // Reset colors
                reset_colors(*strm);

                if (need_trailing_newline)
                    strm->put(static_cast<char_type>('\n'));

                if (m_auto_flush)
                    strm->flush();
            }
        }
    }

    /*!
     * The method flushes all attached streams.
     */
    void flush() {
        for (auto const& stream_ptr : m_streams) {
            stream_type *const strm = stream_ptr.get();
            if (strm->good()) [[likely]]
                strm->flush();
        }
    }

private:
    //! Constructor implementation
    template <typename ArgsT> void construct(ArgsT const& args) {
        construct(
            args[boost::log::keywords::auto_newline_mode | boost::log::sinks::auto_newline_mode::insert_if_missing],
            args[boost::log::keywords::auto_flush | false]);
    }

    //! Constructor implementation
    void construct(auto_newline_mode auto_newline, bool auto_flush) {
        m_auto_newline_mode = auto_newline;
        m_auto_flush        = auto_flush;
    }
};

/**
 * Convenience function to create a colored console log sink
 *
 * This function creates a colored console sink that behaves identically to
 * boost::log::add_console_log() but with severity-based color coding.
 *
 * @param strm Output stream (typically std::cout or std::clog)
 * @return Shared pointer to the created sink
 */
template <typename CharT>
boost::shared_ptr<boost::log::sinks::synchronous_sink<colored_text_ostream_backend<CharT>>>
add_colored_console_log(std::basic_ostream<CharT>& strm) {
    using namespace boost::log;

    // Create shared pointer to stream with null deleter (don't manage stream lifetime)
    auto stream_ptr = boost::shared_ptr<std::basic_ostream<CharT>>(&strm, boost::null_deleter());

    // Create the colored backend
    auto backend = boost::make_shared<colored_text_ostream_backend<CharT>>();
    backend->add_stream(stream_ptr);

    // Create the sink with synchronous frontend
    auto sink = boost::make_shared<sinks::synchronous_sink<colored_text_ostream_backend<CharT>>>(backend);

    // Add sink to the logging core
    boost::log::core::get()->add_sink(sink);

    return sink;
}

// Convenience function for char streams (most common case)
inline auto add_colored_console_log(std::ostream& strm) { return add_colored_console_log<char>(strm); }

} // namespace detail

// management related
namespace detail {

/// Global mutex protecting all logging system operations
/// This single lock eliminates potential deadlock scenarios
/// by ensuring consistent locking order across all functions
static std::mutex log_mutex;

/// Initialization status tracking (protected by log_mutex)
static bool init_status{false};

/// Enum for sink type
enum class sink_type : int {
    console    = 0,
    file       = 1,
    file_error = 2,
    file_debug = 3,
    custom     = 10000,
};

/// FlatMap tracking sink status (protected by log_mutex)
static boost::container::flat_map<sink_type, boost::shared_ptr<sinks::sink>> sink_map;

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
void log_init() {
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    // Check if already initialized
    if (detail::init_status) {
        return;
    }

    // Cleanup work
    {
        // Flush all pending log records before cleanup
        logging::core::get()->flush();

        // Clear our tracking map
        detail::sink_map.clear();

        // Boost.Log will handle ongoing operations gracefully via shared_ptr
        logging::core::get()->remove_all_sinks();
    }

    // Enable logging
    logging::core::get()->set_logging_enabled(true);

    // Add common attributes (timestamp, file, line, function)
    logging::add_common_attributes();

    // Enable filtering, default to INFO level and above
    logging::core::get()->set_filter(expr::attr<severity_level>("Severity") >= severity_level::info);

    // Automatic enable console
    {
        // Setup colored console logging with hierarchical formatting
        auto sink = detail::add_colored_console_log(std::clog);

        sink->set_formatter(expr::format("[%1%][%2%][%3%] %4%") %
                            expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f") %
                            expr::attr<severity_level>("Severity") % expr::attr<std::string>("Channel") %
                            expr::smessage);

        sink->locked_backend()->auto_flush(true);

        // Add sink to boost core
        logging::core::get()->add_sink(sink);

        // Add sink to the map
        detail::sink_map[detail::sink_type::console] = sink;
    }

    detail::init_status = true;
}

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
void log_deinit() noexcept {
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    // Check if already deinitialized
    if (!detail::init_status) {
        return;
    }

    // Flush all pending log records before cleanup
    logging::core::get()->flush();

    // Clear our tracking map
    detail::sink_map.clear();

    // Boost.Log will handle ongoing operations gracefully via shared_ptr
    logging::core::get()->remove_all_sinks();

    // Disable logging
    logging::core::get()->set_logging_enabled(false);

    // Reset status to allow re-initialization
    detail::init_status = false;
}

/**
 * Check if the logging system is initialized
 * @return true if logging system is initialized and ready to use, false otherwise
 */
bool log_is_init() noexcept {
    std::lock_guard<std::mutex> lock(detail::log_mutex);
    return detail::init_status;
}

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
void enable_console() {
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    // Check if logging system is initialized
    if (!detail::init_status) {
        return;
    }

    // Check if console sink already exists
    if (detail::sink_map.find(detail::sink_type::console) != detail::sink_map.end()) {
        return; // Console sink already exists
    }

    // Setup colored console logging with hierarchical formatting
    auto sink = detail::add_colored_console_log(std::clog);

    sink->set_formatter(expr::format("[%1%][%2%][%3%] %4%") %
                        expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f") %
                        expr::attr<severity_level>("Severity") % expr::attr<std::string>("Channel") % expr::smessage);

    sink->locked_backend()->auto_flush(true);

    // Add sink to boost core
    logging::core::get()->add_sink(sink);

    // Add sink to the map
    detail::sink_map[detail::sink_type::console] = sink;
}

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
void disable_console() noexcept {
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    // Check if logging system is initialized
    if (!detail::init_status) {
        return;
    }

    // Check if console sink exists
    auto it = detail::sink_map.find(detail::sink_type::console);
    if (it == detail::sink_map.end()) {
        return; // Console sink doesn't exist
    }

    // Flush all pending log records before removal
    logging::core::get()->flush();

    // Remove the sink from the logging core
    logging::core::get()->remove_sink(it->second);

    // Remove from the map
    detail::sink_map.erase(it);
}

// Note: sim_estab_log() is now fully implemented in the module interface
// using the exported opaque wrapper classes (logger_mt, record, record_ostream)

// ============================================================================
// record implementation
// ============================================================================

// Static assertions to verify storage size
static_assert(sizeof(boost::log::record) <= record::storage_size, "record storage too small for boost::log::record");
static_assert(alignof(boost::log::record) <= record::storage_align,
              "record storage alignment insufficient for boost::log::record");

record::record() noexcept { new (storage_) boost::log::record(); }

record::record(record&& other) noexcept {
    auto *other_impl = static_cast<boost::log::record *>(other.get_impl());
    new (storage_) boost::log::record(std::move(*other_impl));
}

record::~record() noexcept {
    auto *impl = std::launder(reinterpret_cast<boost::log::record *>(storage_));
    std::destroy_at(impl);
}

record& record::operator=(record&& other) noexcept {
    if (this != &other) {
        auto *impl       = static_cast<boost::log::record *>(get_impl());
        auto *other_impl = static_cast<boost::log::record *>(other.get_impl());
        *impl            = std::move(*other_impl);
    }
    return *this;
}

record::operator bool() const noexcept {
    auto *impl = static_cast<const boost::log::record *>(get_impl());
    return static_cast<bool>(*impl);
}

bool record::operator!() const noexcept {
    auto *impl = static_cast<const boost::log::record *>(get_impl());
    return !(*impl);
}

void record::swap(record& other) noexcept {
    auto *impl       = static_cast<boost::log::record *>(get_impl());
    auto *other_impl = static_cast<boost::log::record *>(other.get_impl());
    impl->swap(*other_impl);
}

void record::reset() noexcept {
    auto *impl = static_cast<boost::log::record *>(get_impl());
    impl->reset();
}

void *record::get_impl() noexcept { return static_cast<void *>(storage_); }

const void *record::get_impl() const noexcept { return static_cast<const void *>(storage_); }

// ============================================================================
// record_ostream implementation
// ============================================================================

static_assert(sizeof(boost::log::record_ostream) <= record_ostream::storage_size,
              "record_ostream storage too small for boost::log::record_ostream");
static_assert(alignof(boost::log::record_ostream) <= record_ostream::storage_align,
              "record_ostream storage alignment insufficient for boost::log::record_ostream");

record_ostream::record_ostream() noexcept : attached_record_(nullptr) { new (storage_) boost::log::record_ostream(); }

record_ostream::record_ostream(record& rec) : attached_record_(&rec) {
    auto *rec_impl = static_cast<boost::log::record *>(rec.get_impl());
    new (storage_) boost::log::record_ostream(*rec_impl);
}

record_ostream::~record_ostream() noexcept {
    auto *impl = std::launder(reinterpret_cast<boost::log::record_ostream *>(storage_));
    std::destroy_at(impl);
}

record_ostream::operator bool() const noexcept {
    auto *impl = static_cast<const boost::log::record_ostream *>(get_impl());
    return static_cast<bool>(*impl);
}

bool record_ostream::operator!() const noexcept {
    auto *impl = static_cast<const boost::log::record_ostream *>(get_impl());
    return !(*impl);
}

record& record_ostream::get_record() { return *attached_record_; }

const record& record_ostream::get_record() const { return *attached_record_; }

void record_ostream::attach_record(record& rec) {
    auto *impl     = static_cast<boost::log::record_ostream *>(get_impl());
    auto *rec_impl = static_cast<boost::log::record *>(rec.get_impl());
    impl->attach_record(*rec_impl);
    attached_record_ = &rec;
}

void record_ostream::detach_from_record() noexcept {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    impl->detach_from_record();
    attached_record_ = nullptr;
}

record_ostream& record_ostream::flush() {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    impl->flush();
    return *this;
}

void *record_ostream::get_impl() noexcept { return static_cast<void *>(storage_); }

const void *record_ostream::get_impl() const noexcept { return static_cast<const void *>(storage_); }

// operator<< implementations
record_ostream& record_ostream::operator<<(bool value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(char value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(signed char value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(unsigned char value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(short value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(unsigned short value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(int value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(unsigned int value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(long value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(unsigned long value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(long long value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(unsigned long long value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(float value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(double value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(long double value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(const char *value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(const wchar_t *value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(const void *value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(const std::string& value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(std::string_view value) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << value;
    return *this;
}

record_ostream& record_ostream::operator<<(std::ostream& (*manip)(std::ostream&)) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(std::ios_base& (*manip)(std::ios_base&)) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

// ============================================================================
// logger implementation
// ============================================================================

static_assert(sizeof(detail::channel_logger) <= logger::storage_size, "logger storage too small for channel_logger");
static_assert(alignof(detail::channel_logger) <= logger::storage_align,
              "logger storage alignment insufficient for channel_logger");

logger::logger(const std::string& channel) { new (storage_) detail::channel_logger(keywords::channel = channel); }

logger::logger(const logger& other) {
    auto *other_impl = static_cast<const detail::channel_logger *>(other.get_impl());
    new (storage_) detail::channel_logger(*other_impl);
}

logger::logger(logger&& other) noexcept {
    auto *other_impl = static_cast<detail::channel_logger *>(other.get_impl());
    new (storage_) detail::channel_logger(std::move(*other_impl));
}

logger::~logger() noexcept {
    auto *impl        = static_cast<detail::channel_logger *>(get_impl());
    using logger_type = detail::channel_logger;
    impl->~logger_type();
}

logger& logger::operator=(const logger& other) {
    if (this != &other) {
        auto *impl       = static_cast<detail::channel_logger *>(get_impl());
        auto *other_impl = static_cast<const detail::channel_logger *>(other.get_impl());
        // Use copy-and-swap idiom
        detail::channel_logger tmp(*other_impl);
        impl->swap(tmp);
    }
    return *this;
}

logger& logger::operator=(logger&& other) noexcept {
    if (this != &other) {
        auto *impl       = static_cast<detail::channel_logger *>(get_impl());
        auto *other_impl = static_cast<detail::channel_logger *>(other.get_impl());
        impl->swap(*other_impl);
    }
    return *this;
}

void logger::swap(logger& other) noexcept {
    auto *impl       = static_cast<detail::channel_logger *>(get_impl());
    auto *other_impl = static_cast<detail::channel_logger *>(other.get_impl());
    impl->swap(*other_impl);
}

std::string logger::channel() const {
    auto *impl = static_cast<const detail::channel_logger *>(get_impl());
    return impl->channel();
}

record logger::open_record(severity_level level) {
    auto *impl = static_cast<detail::channel_logger *>(get_impl());
    record rec;
    auto *rec_impl = static_cast<boost::log::record *>(rec.get_impl());
    *rec_impl      = impl->open_record(keywords::severity = level);
    return rec;
}

void logger::push_record(record&& rec) {
    auto *impl     = static_cast<detail::channel_logger *>(get_impl());
    auto *rec_impl = static_cast<boost::log::record *>(rec.get_impl());
    impl->push_record(std::move(*rec_impl));
}

void *logger::get_impl() noexcept { return static_cast<void *>(storage_); }

const void *logger::get_impl() const noexcept { return static_cast<const void *>(storage_); }

// ============================================================================
// logger_mt implementation
// ============================================================================

static_assert(sizeof(detail::channel_logger_mt) <= logger_mt::storage_size,
              "logger_mt storage too small for channel_logger_mt");
static_assert(alignof(detail::channel_logger_mt) <= logger_mt::storage_align,
              "logger_mt storage alignment insufficient for channel_logger_mt");

logger_mt::logger_mt(const std::string& channel) {
    new (storage_) detail::channel_logger_mt(keywords::channel = channel);
}

logger_mt::logger_mt(const logger_mt& other) {
    auto *other_impl = static_cast<const detail::channel_logger_mt *>(other.get_impl());
    new (storage_) detail::channel_logger_mt(*other_impl);
}

logger_mt::logger_mt(logger_mt&& other) noexcept {
    auto *other_impl = static_cast<detail::channel_logger_mt *>(other.get_impl());
    new (storage_) detail::channel_logger_mt(std::move(*other_impl));
}

logger_mt::~logger_mt() noexcept {
    auto *impl        = static_cast<detail::channel_logger_mt *>(get_impl());
    using logger_type = detail::channel_logger_mt;
    impl->~logger_type();
}

logger_mt& logger_mt::operator=(const logger_mt& other) {
    if (this != &other) {
        auto *impl       = static_cast<detail::channel_logger_mt *>(get_impl());
        auto *other_impl = static_cast<const detail::channel_logger_mt *>(other.get_impl());
        // Use copy-and-swap idiom
        detail::channel_logger_mt tmp(*other_impl);
        impl->swap(tmp);
    }
    return *this;
}

logger_mt& logger_mt::operator=(logger_mt&& other) noexcept {
    if (this != &other) {
        auto *impl       = static_cast<detail::channel_logger_mt *>(get_impl());
        auto *other_impl = static_cast<detail::channel_logger_mt *>(other.get_impl());
        impl->swap(*other_impl);
    }
    return *this;
}

void logger_mt::swap(logger_mt& other) noexcept {
    auto *impl       = static_cast<detail::channel_logger_mt *>(get_impl());
    auto *other_impl = static_cast<detail::channel_logger_mt *>(other.get_impl());
    impl->swap(*other_impl);
}

std::string logger_mt::channel() const {
    auto *impl = static_cast<const detail::channel_logger_mt *>(get_impl());
    return impl->channel();
}

record logger_mt::open_record(severity_level level) {
    auto *impl = static_cast<detail::channel_logger_mt *>(get_impl());
    record rec;
    auto *rec_impl = static_cast<boost::log::record *>(rec.get_impl());
    *rec_impl      = impl->open_record(keywords::severity = level);
    return rec;
}

void logger_mt::push_record(record&& rec) {
    auto *impl     = static_cast<detail::channel_logger_mt *>(get_impl());
    auto *rec_impl = static_cast<boost::log::record *>(rec.get_impl());
    impl->push_record(std::move(*rec_impl));
}

void *logger_mt::get_impl() noexcept { return static_cast<void *>(storage_); }

const void *logger_mt::get_impl() const noexcept { return static_cast<const void *>(storage_); }

} // namespace sim_estab::core::log
