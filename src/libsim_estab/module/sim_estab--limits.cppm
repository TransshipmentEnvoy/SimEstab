/**
 * @file sim_estab--limits.cppm
 * @brief Module partition for engine-wide limits
 *
 * Every value here is decided in doc/design_limits.md, which carries the reasoning and
 * the revisit trigger for each one. This partition is the landing site: comments below say
 * what a value binds, and at most summarize why.
 *
 * Two kinds of constant live here and they are not interchangeable:
 *
 * - Build constants - fixed when this library compiles. Retuning is a rebuild, never a
 *   config change: the chunk quantum, the ceilings (max_source_capacity, max_players,
 *   max_sources, max_views, max_paced_participants), and the entity capacities
 *   (default_entity_capacity, max_entity_capacity).
 * - Policy defaults - the shipped default of a field a session may override at
 *   construction (EngineConfig, src/sim_estab/config.py). The engine reads the CONFIGURED
 *   value and never these; they exist so C++ and Python cannot drift, and so a session
 *   that configures nothing still has an answer.
 *
 * What is deliberately ABSENT is as load-bearing as what is here. C, the engine-wide
 * commands executed per tick, is the sum of the configured CAPACITIES over the endpoint
 * set, and that set is not closed until the freeze; ring DEPTH ((margin + 1) x capacity)
 * and the event ring's C x D both follow it. None of the three is a constant, because a
 * constant would be wrong for every session that loads a different number of mods - and
 * because margin is input_delay_ticks, which design_limits.md 7 has not decided.
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <cstddef>
#include <cstdint>

// Module declaration
export module sim_estab:limits;

/**
 * @namespace sim_estab::core::limits
 * @brief Engine-wide limits and the bounds derived from them
 *
 * See doc/design_limits.md, which carries the reasoning and the revisit trigger for
 * every value below.
 */
namespace sim_estab::core::limits {

// ---------------------------------------------------------------------------
// Time (policy default - EngineConfig.tick_rate)
// ---------------------------------------------------------------------------

/// Fixed simulation rate in Hz. One tick is one engine step; the tick counter is the
/// simulation clock. Part of session identity - a replay across rates is rejected by the
/// header. Game speed is set_time_scale() and is a separate, unrecorded concern.
export inline constexpr std::uint32_t default_tick_rate = 30;

// ---------------------------------------------------------------------------
// Command admission (policy defaults - EngineConfig.command_policy)
// design_engine_core.md 5.1, design_python_api.md 7.1
// ---------------------------------------------------------------------------

/// D - the number of ticks between two drain_events() calls that the event ring is sized
/// to buffer. A host exceeding it is a host defect.
export inline constexpr std::uint32_t default_drain_interval_ticks = 8;

/// Commands ONE endpoint may hold for ONE tick, when a manifest declares none. It is the
/// only number an endpoint declares: there is no drain quota, because a tick executes
/// everything stamped for it. Exceeding it is queue_full at the submitting call, and it is
/// the only capacity rejection in the engine.
export inline constexpr std::uint32_t default_source_capacity = 64;

/// The ceiling a manifest may request for its per-endpoint capacity.
export inline constexpr std::uint32_t max_source_capacity = 256;

/// The host endpoint (source 0) gets the ceiling by default. It needs no reserve carved
/// out of an engine-wide pool, because rings are per endpoint: no mod can consume the host's
/// capacity, whatever it submits.
export inline constexpr std::uint32_t default_host_source_capacity = 256;

/// A peer endpoint carries a whole remote player's turn, so it gets the ceiling for the
/// same reason the host does.
export inline constexpr std::uint32_t default_peer_source_capacity = 256;

// ---------------------------------------------------------------------------
// Freeze-time bounds (design_engine_core.md 2.4 step 3a)
//
// Deliberately NOT constants here: C, ring DEPTH, and the event ring's C x D. C is the
// sum of the configured capacities over the endpoint set, and that set is not closed
// until the freeze; DEPTH is (margin + 1) x capacity per endpoint, and margin is
// input_delay_ticks, which design_limits.md 7 has not decided. Writing any of them down
// would create an engine-wide pool, which per-endpoint rings avoid, and would be wrong for
// every session that loads a different number of mods.
// ---------------------------------------------------------------------------

/// Humans a session admits. A game rule, not an engine structure: nothing in the engine is
/// sized by it, and session formation is what enforces it.
export inline constexpr std::uint32_t max_players = 128;

/// Anything holding a command queue - one per originating peer, plus the host, every
/// command-producing mod, and engine/AI sources. Command order is by source id. It exceeds
/// the player count so a full lobby still leaves room for mods beside its players.
export inline constexpr std::uint32_t max_sources = 256;

/// Views a session may register. Publish cost is a sum over due Views, so the number of
/// terms must be knowable at construction; the size of each is measured, not capped. The
/// cap closes the sum rather than restraining a design, which is why it is set well above
/// any plausible session - and why raising it raises the worst-case snapshot memory with
/// it, since that is a sum over the same terms.
export inline constexpr std::uint32_t max_views = 64;

/// Anything the gate waits for, with a declared deadline and an expiry policy. Shares
/// the source ceiling: submitting is what makes a peripheral a participant, so the two sets
/// nearly coincide. They wait concurrently, so the worst case is max(deadline) rather than
/// a sum, and a 256-entry array scanned once per tick costs nothing. This is a server
/// node's worst case, not a client's normal one.
export inline constexpr std::uint32_t max_paced_participants = 256;

// ---------------------------------------------------------------------------
// Storage (build constant - LIBSIM_ESTAB__CHUNK_ELEMENTS)
// design_data_container.md 4
// ---------------------------------------------------------------------------

#ifndef LIBSIM_ESTAB__CHUNK_ELEMENTS
    // Fallback for a build that did not go through this project's CMake. The cache
    // variable is the source of truth; keep the two in step.
    #define LIBSIM_ESTAB__CHUNK_ELEMENTS 1024
#endif

/// The chunk quantum in ELEMENTS. A chunk is a fixed range of row offsets and the unit of
/// parallel-for work items, SIMD iteration and false-sharing isolation - nothing else. It
/// carries no spatial meaning and constrains no GPU workgroup, because uploads are
/// whole-column.
export inline constexpr std::size_t chunk_elements = LIBSIM_ESTAB__CHUNK_ELEMENTS;

/// One cache line's worth of elements of type T - the rule column<T> pads its element
/// count up to (design_data_container.md 3). Stating it in bytes is what makes
/// one rule hold at every ISA level from SSE to AVX-512.
export template <typename T>
inline constexpr std::size_t padding_elements = 64u / sizeof(T);

// ---------------------------------------------------------------------------
// Entity capacity (build constants - [[=cap(N)]], EngineConfig.entity_capacity)
// design_limits.md 4, design_data_container.md 2.1, 2.2
// ---------------------------------------------------------------------------

/// The cap of every object type that declares none: the most entities of that type alive
/// at one time. A ceiling, not a working size - at 64 bytes a row it stops a runaway system
/// at 1 GiB. It costs address space, not memory: columns are reserved at the cap and
/// committed as the pool fills.
export inline constexpr std::uint32_t default_entity_capacity = std::uint32_t{1} << 24;

/// The largest cap a type may declare or a session may configure. A slot is a u32 and the
/// high half of a u64 id, so a larger cap would have no slot to put an entity in; holding
/// the value in a u32 is what enforces that.
export inline constexpr std::uint32_t max_entity_capacity = 0xFFFF'FFFFu;

// ---------------------------------------------------------------------------
// Measured, not capped - the projection warn bandwidth
// design_limits.md 5
//
// Projected-row width has no cap, by decision rather than omission. Its cost is measured
// instead: publish reports duration and bytes/second as first-class metrics, and crossing
// the threshold is a diagnosed warning naming the measured rate - never a rejection, and
// nothing about the simulation changes.
// ---------------------------------------------------------------------------

/// Publish bandwidth above which the engine warns, naming the measured rate AND THE VIEW.
/// Evaluated PER VIEW, never against the session total: every remedy is a change to one
/// declaration - a narrower spec, a tighter row predicate, a lower cadence - so a crossing
/// has to name one. The total is still reported; it is not what this is read against.
///
/// 4 GB/s is reasoned, not measured (design_limits.md 5). One unfiltered View over 10^6
/// live rows at a 32-byte spec is 0.96 GB/s at 30 Hz - the scale this design budgets for -
/// so a threshold near 1 GB/s would warn during ordinary operation. 4 GB/s leaves that case
/// quiet, still catches the same View at 10^7 rows (9.6 GB/s), and stays far under a
/// memcpy-bound machine.
export inline constexpr std::uint64_t default_projection_warn_bytes_per_second = 4'000'000'000;

// ---------------------------------------------------------------------------
// Invariants
//
// These check relationships between the constants, not their values, so any constant may
// be retuned freely. Endpoint capacity and D are configurable at run time, so these check
// only the defaults; configured values are validated in config.py and again at the freeze.
// ---------------------------------------------------------------------------

static_assert(default_tick_rate > 0, "a tick rate of zero has no meaning; the counter is the clock");

static_assert(default_source_capacity <= max_source_capacity,
              "the default capacity must be requestable");
static_assert(default_host_source_capacity <= max_source_capacity,
              "the host endpoint requests its capacity through the same ceiling as any other");
static_assert(default_peer_source_capacity <= max_source_capacity,
              "a peer endpoint requests its capacity through the same ceiling as any other");
static_assert(default_drain_interval_ticks > 0, "D is a count of ticks the engine buffers, never zero");

static_assert(max_sources > max_players,
              "a full lobby allocates one source per peer, so the source ceiling must leave "
              "room for the host and for mods beside the players it was sized for");
static_assert(max_paced_participants <= max_sources,
              "participants share the source ceiling: submitting makes a peripheral a "
              "participant, so the two sets nearly coincide");

static_assert(max_views > 0, "the engine registers a default View, so zero is unrepresentable");
static_assert(max_paced_participants > 0, "the host is always a participant, so zero is unrepresentable");

static_assert(default_entity_capacity > 0, "a cap of zero admits no entity, so no type could exist");
static_assert(default_entity_capacity <= max_entity_capacity,
              "the default cap must be one a type could also declare");

static_assert(chunk_elements >= 64, "a chunk must hold a whole cache line of the narrowest (1-byte) column type");
static_assert((chunk_elements & (chunk_elements - 1)) == 0, "the chunk quantum must be a power of two");
static_assert(chunk_elements % padding_elements<std::uint8_t> == 0,
              "chunk boundaries must land on column-padding boundaries, or a chunk starts mid-vector; "
              "a power of two >= 64 satisfies this for every T up to 8 bytes at once");

} // namespace sim_estab::core::limits
