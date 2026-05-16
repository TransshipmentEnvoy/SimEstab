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
#define SIM_ESTAB_LOG(ch, sev, ...)                                                                                    \
    ::sim_estab::core::log::sim_estab_log(ch, ::sim_estab::core::log::severity_level::sev, __VA_ARGS__)

/**
 * Opt-in adapter for types that already support std::ostream output.
 *
 * Define this in the same namespace as the adapted type so argument-dependent
 * lookup can find the generated record_ostream overload.
 */
#define SIM_ESTAB_LOG_ENABLE_OSTREAM(Type)                                                                             \
    inline ::sim_estab::core::log::record_ostream& operator<<(::sim_estab::core::log::record_ostream& os,              \
                                                              const Type& value) {                                     \
        return ::sim_estab::core::log::stream_via_ostream(os, value);                                                  \
    }

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
#define SIM_ESTAB_LOG_TRACE(ch, ...) SIM_ESTAB_LOG(ch, trace, __VA_ARGS__)

/**
 * Log a debug-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_DEBUG("sim_estab.physics", "Updating physics step " << step_count);
 */
#define SIM_ESTAB_LOG_DEBUG(ch, ...) SIM_ESTAB_LOG(ch, debug, __VA_ARGS__)

/**
 * Log an info-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_INFO("sim_estab.app", "Application started successfully");
 */
#define SIM_ESTAB_LOG_INFO(ch, ...) SIM_ESTAB_LOG(ch, info, __VA_ARGS__)

/**
 * Log a warning-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_WARN("sim_estab.render", "Low memory warning: " << available_mb << " MB remaining");
 */
#define SIM_ESTAB_LOG_WARN(ch, ...) SIM_ESTAB_LOG(ch, warning, __VA_ARGS__)

/**
 * Log an error-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_ERROR("sim_estab.io", "Failed to load file: " << filename << " - " << error_msg);
 */
#define SIM_ESTAB_LOG_ERROR(ch, ...) SIM_ESTAB_LOG(ch, error, __VA_ARGS__)

/**
 * Log a critical-level message to the specified channel
 * @param ch Hierarchical channel name
 * @param msg Message content (supports stream operators)
 *
 * Usage: SIM_ESTAB_LOG_CRITICAL("sim_estab.core", "Fatal error: " << error_description);
 */
#define SIM_ESTAB_LOG_CRITICAL(ch, ...) SIM_ESTAB_LOG(ch, critical, __VA_ARGS__)

#endif // LIBSIM_ESTAB__MACRO_H
