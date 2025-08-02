/**
 * @file log.cpp
 * @brief Implementation of the SimEstab hierarchical logging system
 *
 * This file provides a simplified implementation of the logging system built on
 * top of Boost.Log. Key features implemented here:
 *
 * - Direct channel-based logging without registry or wrapper overhead
 * - Hierarchical channel organization with dot-separated names
 * - Console output sink with timestamp, severity, and channel formatting
 * - Severity level filtering (default: INFO and above)
 * - Exception-safe initialization and cleanup
 * - Zero stringstream usage for optimal performance
 * - Leverages Boost.Log v2 core for efficient logger management
 *
 * The implementation uses Boost.Log directly through macros, eliminating
 * any intermediate layers for maximum performance.
 */

#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/keywords/channel.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>

#include "log.detail.hpp"
#include "log.h"

namespace sim_estab::core::log {

namespace {
namespace logging = boost::log;
namespace sinks   = boost::log::sinks;
namespace expr    = boost::log::expressions;
namespace attrs   = boost::log::attributes;
} // namespace

namespace detail {

/// Mutex protecting logging system initialization/deinitialization
static std::mutex init_mutex;

/// Atomic flag tracking logging system initialization status
static std::atomic<bool> init_status{false};

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
    std::lock_guard<std::mutex> lock(detail::init_mutex);

    // Check if already initialized
    if (detail::init_status.load(std::memory_order_acquire)) {
        return;
    }

    // Add common attributes (timestamp, file, line, function)
    logging::add_common_attributes();

    // Setup colored console logging with hierarchical formatting
    auto sink = detail::add_colored_console_log(std::clog);

    sink->set_formatter(expr::format("[%1%][%2%][%3%] %4%") %
                        expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f") %
                        expr::attr<severity_level>("Severity") % expr::attr<std::string>("Channel") % expr::smessage);

    sink->locked_backend()->auto_flush(true);

    // Enable filtering, default to INFO level and above
    logging::core::get()->set_filter(expr::attr<severity_level>("Severity") >= severity_level::info);

    detail::init_status.store(true, std::memory_order_release);
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
    std::lock_guard<std::mutex> lock(detail::init_mutex);

    // Check if already deinitialized
    if (!detail::init_status.load(std::memory_order_acquire)) {
        return;
    }

    // Flush all pending log records before cleanup
    logging::core::get()->flush();

    // Remove all sinks
    logging::core::get()->remove_all_sinks();

    // Reset status to allow re-initialization
    detail::init_status.store(false, std::memory_order_release);
}

/**
 * Check if the logging system is initialized
 * @return true if logging system is initialized and ready to use, false otherwise
 */
bool log_is_init() noexcept { return detail::init_status.load(std::memory_order_acquire); }

} // namespace sim_estab::core::log
