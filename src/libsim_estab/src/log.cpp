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
#include <ctime>
#include <functional>
#include <iomanip>
#include <ios>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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
} // namespace detail

// management related
namespace detail {

/// Global mutex protecting the logging system's mutable state.
/// Logging is the bottom of the process lock hierarchy: any subsystem may
/// emit while holding its own lock, and nothing reachable from the emission
/// path may take a lock anyone else holds above logging.
/// See design_patterns.md §2 "Lock hierarchy".
static std::mutex log_mutex;

/// Initialization status. Deliberately NOT guarded by log_mutex: log_is_init()
/// sits on every emission path in the process, so taking the mutex there would
/// (a) deadlock log_init() against itself the moment it logs its own lifecycle
/// and (b) serialize every emission in the process on one lock, including
/// inside the parallel engine core update. Written release / read acquire so
/// a caller that observes `true` also observes the fully-built sink state.
static std::atomic<bool> init_status{false};

/// The user's level (design_logging.md §3 "The core filter and set_severity").
/// Protected by log_mutex; log_init resets it to info.
static severity_level user_level = severity_level::info;

/// The line the console prints, inside its colours (colored_text_ostream_backend)
inline auto line_format() {
    return expr::format("[%1%][%2%][%3%] %4%") %
           expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f") %
           expr::attr<severity_level>("Severity") % expr::attr<std::string>("Channel") % expr::smessage;
}

/// A sink filter: `floor <= Severity` (the console)
inline auto floor_filter(severity_level floor) { return expr::attr<severity_level>("Severity") >= floor; }

// ----------------------------------------------------------------------------
// The slot states: the tracking container follows the slot list
// (design_logging.md §3 "Sink slots underneath the toggles")
// ----------------------------------------------------------------------------

using console_sink = sinks::synchronous_sink<colored_text_ostream_backend<char>>;

/// What the subsystem holds for one slot. Specialized per slot; protected by log_mutex.
template <SinkSlot Slot> struct slot_state;

template <> struct slot_state<ConsoleSlot> {
    boost::shared_ptr<console_sink> sink; ///< the one console sink, or null
};

/// The slot list. A new slot adds its type here and its slot_impl below; enable<Slot>
/// and disable<Slot> need no change.
static std::tuple<slot_state<ConsoleSlot>> slots;

template <SinkSlot Slot> slot_state<Slot>& state_of() noexcept { return std::get<slot_state<Slot>>(slots); }

/// The core filter: the user's level (design_logging.md §3 "The core filter and
/// set_severity"). Called under log_mutex after every change to it.
void apply_core_filter() { logging::core::get()->set_filter(floor_filter(user_level)); }

/// The core is enabled only while a sink is registered (design_logging.md §1). With no
/// sink, Boost would write every record to stdout through its default sink. Called under
/// log_mutex after every change to the sinks.
void apply_core_enabled() {
    const bool any = static_cast<bool>(state_of<ConsoleSlot>().sink);
    logging::core::get()->set_logging_enabled(any);
}

/// Flush, forget every sink and remove them from the core (under log_mutex)
void clear_slots() {
    logging::core::get()->flush();
    state_of<ConsoleSlot>().sink.reset();
    logging::core::get()->remove_all_sinks();
}

// ----------------------------------------------------------------------------
// slot_impl: what enable<Slot> and disable<Slot> need from each slot. The Boost
// backend, the formatter and the filter of a slot live here and nowhere else.
// ----------------------------------------------------------------------------

template <SinkSlot Slot> struct slot_impl;

template <> struct slot_impl<ConsoleSlot> {
    using sink_type = console_sink;

    static bool has(const slot_state<ConsoleSlot>& state, const ConsoleSlot::params&) noexcept {
        return static_cast<bool>(state.sink);
    }

    /// The colored backend on std::clog, through a non-owning pointer
    static boost::shared_ptr<sink_type> open(const ConsoleSlot::params&) {
        auto stream  = boost::shared_ptr<std::ostream>(&std::clog, boost::null_deleter());
        auto backend = boost::make_shared<colored_text_ostream_backend<char>>();
        backend->add_stream(stream);
        backend->auto_flush(ConsoleSlot::auto_flush);
        auto sink = boost::make_shared<sink_type>(backend);
        sink->set_formatter(line_format());
        sink->set_filter(floor_filter(user_level));
        return sink;
    }

    static void add(slot_state<ConsoleSlot>& state, const ConsoleSlot::params&, boost::shared_ptr<sink_type> sink) {
        state.sink = std::move(sink);
    }

    static boost::shared_ptr<sink_type> take(slot_state<ConsoleSlot>& state, const ConsoleSlot::key&) noexcept {
        return std::exchange(state.sink, nullptr);
    }
};

} // namespace detail

// ============================================================================
// The slot pair (design_logging.md §3): one body per operation, instantiated per slot
// ============================================================================

template <SinkSlot Slot> void enable(const typename Slot::params& params) {
    using impl = detail::slot_impl<Slot>;
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    // Toggle shape (design_patterns.md §2): a no-op when not initialized, a no-op on a
    // sink that is already enabled
    if (!detail::init_status.load(std::memory_order_acquire)) {
        return;
    }
    auto& state = detail::state_of<Slot>();
    if (impl::has(state, params)) {
        return;
    }

    // A failed open throws here, before anything is registered
    auto sink = impl::open(params);
    logging::core::get()->add_sink(sink);
    impl::add(state, params, std::move(sink));
    detail::apply_core_enabled();
    detail::apply_core_filter();
}

template <SinkSlot Slot> void disable(const typename Slot::key& key) noexcept {
    using impl = detail::slot_impl<Slot>;
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    if (!detail::init_status.load(std::memory_order_acquire)) {
        return;
    }
    auto sink = impl::take(detail::state_of<Slot>(), key);
    if (!sink) {
        return;
    }

    // Flush all pending log records before removal
    logging::core::get()->flush();
    logging::core::get()->remove_sink(sink);
    detail::apply_core_enabled();
    detail::apply_core_filter();
}

template void enable<ConsoleSlot>(const ConsoleSlot::params&);
template void disable<ConsoleSlot>(const ConsoleSlot::key&) noexcept;

// ============================================================================
// The triad and the named pairs
// ============================================================================

/**
 * Initialize the hierarchical logging system
 *
 * Removes every sink from the process's one Boost.Log core, sets the user's level to info
 * and the core filter with it, adds the console sink and enables the core, which stays
 * enabled while a sink is registered. Thread-safe and idempotent: a call while
 * initialized does nothing.
 *
 * @throws May throw boost::log exceptions if system resources are insufficient
 */
void log_init() {
    {
        std::lock_guard<std::mutex> lock(detail::log_mutex);

        // Check if already initialized
        if (detail::init_status.load(std::memory_order_acquire)) {
            return;
        }

        // Cleanup work: whatever sinks the core holds, ours or another user's
        detail::clear_slots();

        // Add common attributes (timestamp, process id, thread id, line id)
        logging::add_common_attributes();

        // The user's level starts at info; the core filter follows it
        detail::user_level = severity_level::info;
        detail::apply_core_filter();

        // The console, automatically. enable<ConsoleSlot> would return early here, since
        // the init flag is still down, so the slot is opened directly.
        {
            using console = detail::slot_impl<ConsoleSlot>;
            auto sink     = console::open({});
            logging::core::get()->add_sink(sink);
            console::add(detail::state_of<ConsoleSlot>(), {}, std::move(sink));
        }

        // Enable logging: the console is registered
        detail::apply_core_enabled();

        // Release: a caller that observes `true` also observes the sinks above.
        detail::init_status.store(true, std::memory_order_release);
    }

    // Lifecycle record, emitted AFTER the lock is released and the flag is set:
    // before that the guard would drop it, and inside the lock this call would
    // be re-entrant if log_is_init() ever took the mutex again.
    // design_patterns.md §7 requires the record; the lock hierarchy of §2 is
    // why the ordering, not an exemption, is what makes it possible.
    sim_estab_log("sim_estab.log", severity_level::info, "Logging subsystem initialized");
}

/**
 * Deinitialize the logging system
 *
 * Emits the last lifecycle record, clears the init flag, flushes and removes every
 * sink, and disables the core. Thread-safe, idempotent and noexcept.
 *
 * @note After calling this function, log_init() must be called again
 *       before any logging operations can be performed.
 */
void log_deinit() noexcept {
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    // Check if already deinitialized
    if (!detail::init_status.load(std::memory_order_acquire)) {
        return;
    }

    // Lifecycle record first: the guard is still true and the sinks are still
    // attached, so this is the last record the subsystem emits about itself.
    sim_estab_log("sim_estab.log", severity_level::info, "Logging subsystem shutting down");

    // Clear the flag FIRST so no new emission starts against state that is
    // about to be torn down (design_patterns.md §2). The guard is advisory:
    // a record already past its check may still be dropped, and log_deinit is
    // externally synchronized against emission rather than draining it.
    detail::init_status.store(false, std::memory_order_release);

    // Flush, forget and remove every sink
    detail::clear_slots();

    // Disable logging
    logging::core::get()->set_logging_enabled(false);
}

/**
 * Check if the logging system is initialized
 * @return true if logging system is initialized and ready to use, false otherwise
 */
bool log_is_init() noexcept {
    // No lock: see the note on detail::init_status. Advisory by contract —
    // it may go stale the instant it returns, so it gates a best-effort
    // emission and never a decision that outlives the call.
    return detail::init_status.load(std::memory_order_acquire);
}

void enable_console() { enable<ConsoleSlot>({}); }

void disable_console() noexcept { disable<ConsoleSlot>({}); }

void set_severity(severity_level level) {
    std::lock_guard<std::mutex> lock(detail::log_mutex);

    if (!detail::init_status.load(std::memory_order_acquire)) {
        return;
    }
    detail::user_level = level;
    if (const auto& console = detail::state_of<ConsoleSlot>().sink) {
        console->set_filter(detail::floor_filter(level));
    }
    detail::apply_core_filter();
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

record_ostream& record_ostream::operator<<(decltype(std::setprecision(0)) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::setw(0)) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::setfill(' ')) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::setbase(10)) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::setiosflags(std::ios_base::fmtflags{})) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::resetiosflags(std::ios_base::fmtflags{})) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::put_money(std::declval<long double>())) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::put_money(std::declval<const std::string&>())) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::put_time(static_cast<const std::tm *>(nullptr), "")) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::quoted("")) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::quoted(std::declval<const std::string&>())) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::quoted(std::declval<std::string&>())) manip) {
    auto *impl = static_cast<boost::log::record_ostream *>(get_impl());
    *impl << manip;
    return *this;
}

record_ostream& record_ostream::operator<<(decltype(std::quoted(std::declval<std::string_view>())) manip) {
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
