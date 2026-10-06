/**
 * @file sim_estab--engine.cppm
 * @brief Module partition for the engine
 *
 * The engine drives the mechanisms of the boundary. This partition holds the tick loop: the
 * loop that passes the gate, runs one tick and repeats (design_engine_core.md §3.3). The
 * gate itself is in sim_estab:engine.gate.
 *
 * Only the tick type is exported, as engine::Tick. The tick loop is internal, and its tests
 * are units of the module (design_patterns.md §10).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <chrono>
#include <cstdint>
#include <functional>

// Module declaration
export module sim_estab:engine;

import :types;
import :engine.gate;

/**
 * @namespace sim_estab::core::engine
 * @brief The engine: the tick loop
 */
namespace sim_estab::core::engine {

/// The tick, under the engine's name. Its home is sim_estab:types.
export using types::Tick;

/// A step checks for an interrupt at least once per slice: at a tick boundary, and between
/// slices while it waits at the gate (design_python_api.md §4.3)
inline constexpr std::chrono::milliseconds interrupt_check_slice{100};

/// What ended a step, or refused it
enum class StepEnd : std::uint32_t {
    GrantEnded,   ///< the granted ticks have run, or a pause ended the grant early
    Stopped,      ///< a stop was requested
    EventBacklog, ///< the event backlog reached its mark; the caller drains and steps again
    Interrupted,  ///< the interrupt check asked the step to end
    Failed,       ///< a participant's deadline passed under OnExpiry::Fail
    StepInFlight, ///< refused: another step is in flight
    Overflow,     ///< refused: the tick count does not fit
};

/// How a step ended
struct StepResult {
    StepEnd end = StepEnd::GrantEnded;
    Tick tick_reached;             ///< the last tick that has run
    std::uint32_t participant = 0; ///< Failed: the participant whose deadline passed
};

/**
 * Run a step on the calling thread, which is the executor until the call returns
 * (design_engine_core.md §3.3)
 *
 * The loop passes the gate, runs one tick and repeats. It ends when the grant of
 * `tick_count` ticks is used up or a pause ends it early, at a stop, and at the event
 * backlog mark. It waits only for a participant that is not ready, up to its deadline.
 * Whatever ends the step, no tick is cut short, and what is left of the grant is revoked.
 *
 * @param tick_gate the session's gate
 * @param tick_count how many ticks to run
 * @param run_tick runs one tick: the command drain, the tick itself and the publish
 * @param is_interrupted true if the step should end at this boundary. Called at most once
 *        per interrupt_check_slice.
 */
[[nodiscard]] StepResult run_step(gate::TickGate& tick_gate, std::uint64_t tick_count,
                                  std::function_ref<void(Tick)> run_tick, std::function_ref<bool()> is_interrupted);

} // namespace sim_estab::core::engine
