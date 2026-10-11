/**
 * @file sim_estab.cppm
 * @brief Main module interface for SimEstab library
 *
 * This is the primary module interface that re-exports all SimEstab subsystems.
 * Consumers can import this module to gain access to the entire library API.
 *
 * Usage:
 *   import sim_estab;             // Import the module
 *   #include <sim_estab/macro.h>  // Include macro headers as needed
 *
 * Note: Macros (like SIM_ESTAB_LOG_*) must still be included from headers
 * as C++20 modules cannot export preprocessor macros.
 */

export module sim_estab;

// Re-export all submodules. The engine's mechanism partitions export only the value types
// a caller handles; the mechanisms themselves stay unexported (design_patterns.md §1).
export import :error;
export import :limits;
export import :log;
export import :gpu;
export import :util;
export import :types;
export import :engine.view;
export import :engine.event_ring;
export import :engine.gate;
export import :engine.endpoint;
export import :engine;

/**
 * @namespace sim_estab
 * @brief Root namespace for the SimEstab simulation framework
 *
 * SimEstab is a hybrid Python/C++ simulation framework providing:
 * - High-performance C++ core with modern C++26 features
 * - Python bindings via nanobind
 * - Modular architecture with C++20 modules
 * - Comprehensive logging system
 * - GPU compute capabilities
 * - Cross-platform support
 *
 * The library is organized into the following namespaces:
 * - sim_estab::core::error - Root of the exception family
 * - sim_estab::core::log - Hierarchical logging system
 * - sim_estab::core::gpu - Headless GPU compute context
 * - sim_estab::core::util - General-purpose utilities (thread_util, ...)
 * - sim_estab::core::types - Strong types of function signatures (Tick)
 * - sim_estab::core::engine - The engine: the tick loop, and the value types of its
 *   mechanisms (engine::view, engine::event_ring, engine::gate, engine::endpoint)
 * - (Additional subsystems to be added as modules are migrated)
 */
