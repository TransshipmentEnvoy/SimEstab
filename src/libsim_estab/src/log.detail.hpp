#pragma once

#include <boost/core/null_deleter.hpp>
#include <boost/log/attributes/attribute_name.hpp>
#include <boost/log/attributes/attribute_value.hpp>
#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/core.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>

#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include <rang.hpp>

#include "log.h"

namespace sim_estab::core::log::detail {

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
class colored_text_ostream_backend : public boost::log::sinks::basic_text_ostream_backend<CharT> {
public:
    //! Base type
    typedef boost::log::sinks::basic_text_ostream_backend<CharT> base_type;

    //! Character type
    typedef typename base_type::char_type char_type;

    //! String type
    typedef typename base_type::string_type string_type;

    //! Stream type
    typedef typename base_type::stream_type stream_type;

    /**
     * Default constructor
     */
    colored_text_ostream_backend() : base_type() {}

    /**
     * Constructor with parameters
     * Forwards all arguments to the base class constructor
     */
    template <typename... ArgsT>
    explicit colored_text_ostream_backend(ArgsT&&...args) : base_type(std::forward<ArgsT>(args)...) {}

    /*!
     * The method adds a new stream to the sink.
     *
     * \param strm Pointer to the stream. Must not be NULL.
     */
    void add_stream(boost::shared_ptr<stream_type> const& strm) {
        base_type::add_stream(strm);
        this->stream_weakref = strm; // Store weak reference to the stream
    }
    /*!
     * The method removes a stream from the sink. If the stream is not attached to the sink,
     * the method has no effect.
     *
     * \param strm Pointer to the stream. Must not be NULL.
     */
    void remove_stream(boost::shared_ptr<stream_type> const& strm) {
        base_type::remove_stream(strm);
        this->stream_weakref.reset(); // Clear weak reference to the stream
    }

    /**
     * The method writes the message to the sink with color coding
     *
     * This method:
     * 1. Extracts the severity level from the log record
     * 2. Applies appropriate colors based on severity (except for INFO)
     * 3. Calls the base implementation to write the formatted message
     * 4. Resets colors to default state
     *
     * @param rec Log record view containing all log attributes
     * @param formatted_message The pre-formatted message string
     */
    void consume(boost::log::record_view const& rec, string_type const& formatted_message) {
        // Extract severity level from the record using attribute name
        boost::log::attribute_value severity_attr = rec["Severity"];

        if (severity_attr) {
            severity_level level = severity_attr.extract<severity_level>().get();
            auto strm            = stream_weakref.lock();

            apply_severity_colors(*strm, level);
            base_type::consume(rec, formatted_message);
            reset_colors(*strm);
        } else {
            // No severity attribute found, just pass through to base class
            base_type::consume(rec, formatted_message);
        }
    }

private:
    typename boost::weak_ptr<stream_type> stream_weakref;
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

} // namespace sim_estab::core::log::detail