#ifndef LIBSIM_ESTAB__MACRO_H
#define LIBSIM_ESTAB__MACRO_H

/**
 * Core logging macro with explicit channel and serverity
 *
 * This is the fundamental logging macro that all other logging macros build upon.
 * It provides direct channel-based logging using Boost.Log's streaming interface
 * without any stringstream overhead.
 *
 * @param ch Hierarchical channel name (e.g., "sim_estab.network.tcp")
 * @param sev Severity level (enum value)
 * @param msg Message content - supports stream operators for formatting
 *
 * Usage:
 *   SIM_ESTAB_LOG("my.channel", debug, "Processing file " << filename << " with " << count << " items");
 */
#define SIM_ESTAB_LOG_SEV(ch, sev, msg)                                                                                \
    do {                                                                                                               \
        if (::sim_estab::core::log::log_is_init()) {                                                                   \
            ::sim_estab::core::log::detail::channel_logger_mt logger(::boost::log::keywords::channel = (ch));          \
            BOOST_LOG_SEV(logger, sev) << msg;                                                                         \
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

#endif // LIBSIM_ESTAB__MACRO_H