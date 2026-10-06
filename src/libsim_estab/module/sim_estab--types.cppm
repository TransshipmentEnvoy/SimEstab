/**
 * @file sim_estab--types.cppm
 * @brief Module partition for the strong types
 *
 * A tick, a source id, a column id and an entity id are strong types in function
 * signatures, so a swapped argument is a compile error (design_patterns.md §10). The
 * structs of the normative sections keep the raw integers their docs write.
 *
 * Only the tick exists yet. Each id arrives with the code that uses it.
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <compare>
#include <cstdint>

// Module declaration
export module sim_estab:types;

/**
 * @namespace sim_estab::core::types
 * @brief The strong types of function signatures
 */
export namespace sim_estab::core::types {

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

} // namespace sim_estab::core::types
