/**
 * @file sim_estab--error.cppm
 * @brief Module partition for the root of the library's exception family
 *
 * Every exception the library throws derives from sim_estab_error, so one catch clause
 * covers all of them. Each subsystem exports its own error, derived from the root:
 * log_error, gpu_error, viz_error (design_patterns.md §6).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <stdexcept>
#include <string>

// Module declaration
export module sim_estab:error;

/**
 * @namespace sim_estab::core::error
 * @brief The library's exception family
 */
namespace sim_estab::core::error {

/**
 * Root of every exception the library throws
 *
 * A subsystem throws its own derived class, never the root. Catch the root to handle
 * any library error without naming the subsystem it came from.
 */
export class sim_estab_error : public std::runtime_error {
public:
    explicit sim_estab_error(const std::string& what) : std::runtime_error(what) {}
};

} // namespace sim_estab::core::error
