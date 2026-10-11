/**
 * @file sim_estab--engine.tick.cppm
 * @brief Module partition for the tick, the engine's clock
 *
 * A tick is a strong type in function signatures, so a swapped argument is a compile error
 * (design_patterns.md §10). The structs of the normative sections keep the raw integer
 * their docs write. Every partition of the engine family imports this leaf; sim_estab:engine
 * exports the tick under the engine's name.
 *
 * The other strong types arrive where they are used: the source id with the endpoint, the
 * column and entity ids with the world container (M3).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <compare>
#include <cstdint>

// Module declaration
export module sim_estab:engine.tick;

/**
 * @namespace sim_estab::core::engine::tick
 * @brief The tick: the only simulation clock, as a strong type
 */
export namespace sim_estab::core::engine::tick {

/// A tick: one step of the simulation clock (design_engine_core.md §3). Built as
/// `Tick{5}`; a bare integer does not convert to it.
struct Tick {
    std::uint64_t value = 0;

    friend constexpr auto operator<=>(Tick, Tick) noexcept = default;
};

/// The tick `count` ticks after `tick`
[[nodiscard]] constexpr Tick operator+(Tick tick, std::uint64_t count) noexcept { return Tick{tick.value + count}; }

/// The tick `count` ticks before `tick`
[[nodiscard]] constexpr Tick operator-(Tick tick, std::uint64_t count) noexcept { return Tick{tick.value - count}; }

} // namespace sim_estab::core::engine::tick
