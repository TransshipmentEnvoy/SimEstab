/**
 * @file sim_estab.cppm
 * @brief Main module interface for SimEstab library
 *
 * This is the primary module interface that re-exports all SimEstab subsystems.
 * Consumers can import this module to gain access to the entire library API.
 *
 * Usage:
 *   import sim_estab;           // Import the module
 *   #include <sim_estab/log.h>  // Include macro headers as needed
 *
 * Note: Macros (like SIM_ESTAB_LOG_*) must still be included from headers
 * as C++20 modules cannot export preprocessor macros.
 */

export module sim_estab;

// Re-export all submodules
export import :log;
export import :gpu;
export import :util;

/**
 * @namespace sim_estab
 * @brief Root namespace for the SimEstab simulation framework
 *
 * SimEstab is a hybrid Python/C++ simulation framework providing:
 * - High-performance C++ core with modern C++23 features
 * - Python bindings via nanobind
 * - Modular architecture with C++20 modules
 * - Comprehensive logging system
 * - GPU compute capabilities
 * - Cross-platform support
 *
 * The library is organized into the following namespaces:
 * - sim_estab::core::log - Hierarchical logging system
 * - sim_estab::core::gpu - Headless GPU compute context
 * - sim_estab::core::util - General-purpose utilities (thread_util, ...)
 * - (Additional subsystems to be added as modules are migrated)
 */
