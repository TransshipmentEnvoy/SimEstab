/**
 * @file sim_estab--engine.endpoint.cppm
 * @brief Module partition for command endpoints; only its value types are exported
 *
 * An endpoint carries one source's commands to the executor (design_engine_core.md §5.1).
 * Its command ring is a single-producer, single-consumer ring: the producer submits on its
 * own thread, and the executor drains every endpoint at the start of each tick, in
 * ascending source id. The endpoint adds the endpoint lease, the admission wait, and the
 * outcome slots its producer reads outcomes from.
 *
 * A paced endpoint's commands name the tick they act on. An unpaced endpoint's commands, the
 * host's and a logic mod's, name none: they run at the first tick whose drain finds them.
 * Until M2 defines the command payload, a command is its tick and opaque bytes, and does
 * nothing (design_engine_core.md §7 step 1).
 *
 * The mechanism register rows (design_engine_core.md §1.1):
 *
 * The command ring
 * - Atomic objects: `write` and `read`, 8 bytes each, each on its own cache line, always
 *   lock-free on x86-64.
 * - Memory orders: submit fills the slot, then stores `write` with release (C5) (H3); the
 *   drain loads it with acquire (S1). The drain stores `read` with release (S4), under the
 *   endpoint's mutex when paced; submit loads it with acquire (C3) (H1), or relaxed under
 *   the mutex when it re-checks there.
 * - Linearization point: submit, at the release store of `write`.
 * - Progress: an unpaced submit is wait-free. A paced submit is wait-free unless its ring is
 *   full; then it parks on the endpoint's mutex until the oldest tick in the ring has run.
 *   The drain is wait-free, plus a short lock per paced endpoint it frees space in.
 * - Failure: every rejection is returned at once, and enqueues nothing.
 *
 * The endpoint lease
 * - Atomic objects: `lease_state` and `active_submit`, 4 bytes each, always lock-free on
 *   x86-64.
 * - Memory orders: a submit loads `lease_state` with acquire, adds to `active_submit` with
 *   acq_rel, and loads `lease_state` again with acquire. revoke() exchanges `lease_state`
 *   with acq_rel under `gate.m` and the endpoint's `m`, and reads `active_submit` only with
 *   a read-modify-write, so it cannot miss a lease that saw OPEN.
 * - Linearization point: a lease, at the second load of `lease_state`; revocation, at the
 *   exchange to REVOKING.
 * - Progress: the submit side is wait-free. The revoker parks on the endpoint's mutex until
 *   the running submits leave, bounded by its deadline.
 * - Failure: a deadline that passes leaves the endpoint REVOKING. The caller takes the
 *   failed path, and reclaims nothing.
 *
 * The command outcome
 * - Atomic objects: none of its own. It rides `read` and the gate's `first_unexecuted`.
 * - Memory orders: the drain writes an outcome's tick before its release store of `read`
 *   (S4), and its type before the executor's release store of `first_unexecuted` (G5). The
 *   producer loads each with acquire before it reads the matching field.
 * - Linearization point: (G5) of the tick that drained the command.
 * - Progress: outcome() is wait-free. wait_outcome() parks on the tick-progress wait until
 *   the tick has run: bounded by structure, not by a timer.
 * - Failure: a handle whose slot has been reused reads Expired.
 *
 * Every park here follows the wake-up rule of design_engine_core.md §3.3, except the
 * revoker's wait for running submits. That is a lease's wait for its running holders, the
 * rule's one stated exception, and it rests on the read-modify-write above. The lock order
 * is `gate.m`, then the endpoint's `m`, and only revoke() holds both.
 *
 * This partition exports only the value types a producer handles: a command, its handle,
 * its outcome and a rejection. The mechanism itself is not exported: an importer of
 * sim_estab cannot name an endpoint. Its tests are units of the module
 * (design_patterns.md §1, §10).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>

// Module declaration
export module sim_estab:engine.endpoint;

import :engine.tick;
import :limits;
import :engine.gate;

/**
 * @namespace sim_estab::core::engine::endpoint
 * @brief Command endpoints: the command ring, the endpoint lease and command outcomes
 */
namespace sim_estab::core::engine::endpoint {

using tick::Tick;

/// The bytes of a command's payload. Provisional: the slot size waits for the M2 payload
/// schema (open_question.md Q22). 56 bytes make a Command one cache line.
export inline constexpr std::size_t provisional_command_payload_bytes = 56;

/**
 * One command, as it sits in a ring slot (design_engine_core.md §5.1)
 *
 * Opaque bytes until M2 defines the command payload (design_engine_core.md §7 step 1).
 */
export struct Command {
    std::uint64_t tick = 0; ///< the tick it acts on; unused on an unpaced endpoint
    std::array<std::byte, provisional_command_payload_bytes> payload{};

    bool operator==(const Command&) const = default;
};

/// Why submit() refused a command (design_engine_core.md §5.1). Invalid arrives with the
/// payload schema (M2) and capabilities (M7), and HostError with process hosts (M7).
export enum class Rejection : std::uint32_t {
    QueueFull,  ///< more than `capacity` commands for one tick; unpaced: waiting for the next drain
    TooLate,    ///< the named tick is past, or this producer has declared it ready
    OutOfOrder, ///< the named tick is earlier than one already named here
    OverMargin, ///< the named tick is further ahead than the endpoint's margin
    Revoked,    ///< the endpoint is not admitting
};

/// What a producer reads a command's outcome with: the endpoint's source and the command's
/// position in its ring
export struct CommandHandle {
    std::uint32_t source   = 0;
    std::uint64_t position = 0; ///< the ring's `write` before the command's submit

    bool operator==(const CommandHandle&) const = default;
};

/// What happened to a command (design_engine_core.md §5.1)
export enum class CommandOutcomeType : std::uint32_t {
    Pending, ///< the tick that runs it has not run yet
    Applied, ///< it ran
    Dropped, ///< the drain refused it: its tick was past (S3)
    Expired, ///< its producer has submitted `depth` commands since, and its slot is reused
};

/// A command's outcome, and the tick it concerns
export struct CommandOutcome {
    CommandOutcomeType type = CommandOutcomeType::Pending;
    /// The tick that ran or dropped it. Pending: the tick it names, or once drained the tick
    /// that drained it; 0 for an unpaced command not drained yet. Expired: 0.
    Tick tick;

    bool operator==(const CommandOutcome&) const = default;
};

/// Why wait_outcome() returned without an outcome
enum class OutcomeWaitRefusal : std::uint32_t {
    Unreleased, ///< this producer has not declared the command's tick ready: the deadlock rule
    Revoked,    ///< the endpoint was revoked while the command was pending
};

/// The endpoint lease's state (design_engine_core.md §5.1)
enum class LeaseState : std::uint32_t { OPEN, REVOKING, REVOKED };

/**
 * The command ring: one endpoint's slots and its two indices (design_engine_core.md §5.1)
 *
 * The producer owns `write` and the executor owns `read`, each on its own cache line.
 */
struct CommandRing {
    std::unique_ptr<Command[]> slot; ///< `depth` commands
    std::uint64_t depth;             ///< capacity, or 2 x capacity for a paced endpoint with a margin
    alignas(limits::cache_line_bytes) std::atomic<std::uint64_t> write; ///< producer-written: commands submitted
    alignas(limits::cache_line_bytes) std::atomic<std::uint64_t> read;  ///< executor-written: commands drained

    explicit CommandRing(std::uint64_t ring_depth)
        : slot(std::make_unique<Command[]>(ring_depth)), depth(ring_depth), write(0), read(0) {}

    CommandRing(const CommandRing&)            = delete;
    CommandRing& operator=(const CommandRing&) = delete;
};

/**
 * One source's endpoint (design_engine_core.md §5.1)
 *
 * Four roles use it. The producer, one thread at a time, submits and reads outcomes. The
 * executor drains it with drain_commands(). The lifecycle opens and revokes it. Whoever
 * builds it owns it and keeps the gate alive for as long. A field marked private to a role
 * is touched by that role only.
 *
 * It starts REVOKED: an endpoint is allocated at the freeze and opened when the session
 * starts.
 */
struct Endpoint {
    using Clock = gate::TickGate::Clock;

    CommandRing ring;
    std::unique_ptr<CommandOutcome[]> outcome_slot; ///< `ring.depth` outcomes, parallel to the ring
    gate::TickGate& tick_gate;                      ///< the session's gate
    std::uint32_t source;                           ///< the source id; the drain visits endpoints in its order
    std::uint32_t capacity;                         ///< commands this endpoint may hold for one tick
    std::uint32_t margin;                           ///< how far past the current tick it may stamp; 0 if unpaced
    bool unpaced;                                   ///< its commands name no tick
    std::uint32_t participant;                      ///< paced: its participant at the gate; 0 if unpaced

    std::atomic<LeaseState> lease_state; ///< the endpoint lease
    std::atomic<std::uint32_t> active_submit;
    std::mutex m;                       ///< held for loads, stores and notifies only
    std::condition_variable space_cv;   ///< a paced producer waiting for ring space parks here
    std::condition_variable revoke_cv;  ///< revoke() waiting for running submits parks here
    std::uint32_t space_waiters    = 0; ///< guarded by m
    std::uint64_t last_stamped     = 0; ///< producer-private: the latest tick named here
    std::uint32_t stamped_for_tick = 0; ///< producer-private: commands named for last_stamped

    /// An unpaced endpoint for `source_id`, holding `per_tick_capacity` commands
    Endpoint(gate::TickGate& session_gate, std::uint32_t source_id, std::uint32_t per_tick_capacity);

    /// A paced endpoint for `source_id`, whose ticks participant `participant_index` releases.
    /// It holds `per_tick_capacity` commands per tick, for up to `stamp_margin` ticks ahead.
    Endpoint(gate::TickGate& session_gate, std::uint32_t source_id, std::uint32_t per_tick_capacity,
             std::uint32_t participant_index, std::uint32_t stamp_margin);

    Endpoint(const Endpoint&)            = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    /// Lifecycle: start admitting. The endpoint must be REVOKED: new, or revoked by a
    /// revoke() that returned true. The old producer must have been joined. The ring keeps
    /// what it holds, so a reopened endpoint drains its admitted commands before new ones.
    void open();

    /**
     * Lifecycle: stop admitting, wake every parked producer, and wait for running submits
     *
     * @return true if every running submit has left, and the endpoint is REVOKED. false if
     *         `until` passed first: the endpoint stays REVOKING, and the caller takes the
     *         failed path without reclaiming it.
     *
     * After true, no submit writes the ring again. The last submit out may still notify,
     * and outcome reads and the drain go on, so freeing or reopening the endpoint waits
     * for the producer's join (design_engine_core.md §5.1).
     */
    [[nodiscard]] bool revoke(Clock::time_point until);

    /// Producer, unpaced: submit `command` for the next drain. Never waits. (H0) to (H3)
    [[nodiscard]] std::expected<CommandHandle, Rejection> submit(const Command& command);

    /// Producer, paced: submit `command` for `tick`. Waits for space if the ring is full,
    /// until the oldest tick in it has run. (C0) to (C5)
    [[nodiscard]] std::expected<CommandHandle, Rejection> submit(const Command& command, Tick tick);

    /// Producer: the outcome of the command `handle` names. Never waits.
    [[nodiscard]] CommandOutcome outcome(const CommandHandle& handle) const;

    /**
     * Producer: wait until the command `handle` names has an outcome, at most until `until`
     *
     * @return the outcome, Pending if `until` passed first. Unreleased: a paced producer
     *         asked about a tick it has not declared ready, which would never run.
     *         Revoked: the endpoint was revoked with the command still pending.
     */
    [[nodiscard]] std::expected<CommandOutcome, OutcomeWaitRefusal> wait_outcome(const CommandHandle& handle,
                                                                                 Clock::time_point until);

    /// Producer: enter the lease. false: the endpoint is not admitting.
    [[nodiscard]] bool try_enter() noexcept;

    /// Producer: leave the lease. The last to leave a closing endpoint wakes the revoker.
    void leave() noexcept;
};

/**
 * Executor: the command drain at the start of `tick` (design_engine_core.md §5.1)
 *
 * It visits `endpoints` in ascending source id, and takes every command for `tick` from
 * each, in FIFO order. `emit` receives each with its source and its sequence, the
 * position in this drain. A command stamped for a later tick stays, with everything behind
 * it. A command stamped for a past tick is dropped, and the drain raises one
 * command.protocol_error per endpoint per tick.
 *
 * The endpoints must share one gate, and be in strictly ascending source id.
 */
void drain_commands(std::span<Endpoint *const> endpoints, Tick tick,
                    std::function_ref<void(const Command&, std::uint32_t, std::uint64_t)> emit);

} // namespace sim_estab::core::engine::endpoint
