/**
 * @file sim_estab--engine.gate.cppm
 * @brief Non-exported module partition for the gate
 *
 * The gate is the one place the engine core waits: before each tick, until nothing blocks
 * the tick (design_engine_core.md §3.3). This partition holds the gate's state, the calls
 * that change a gate input, the executor's wait before a tick, and the wait for a tick to
 * run. The tick loop that passes the gate is in sim_estab:engine.
 *
 * This is the gate at its simplest setting (design_engine_core.md §7 step 1): the executor
 * is the thread inside a step. It waits only for a participant. At a stop, a pause or the
 * event backlog mark it returns to its caller.
 *
 * The mechanism register rows (design_engine_core.md §1.1):
 *
 * The gate
 * - Atomic objects: each participant's `ready_through` (8 bytes) and `active` (4 bytes),
 *   and the host's `run_until` (8 bytes) and `stop_requested` (4 bytes). Each is naturally
 *   aligned and always lock-free on x86-64.
 * - Memory orders: a writer stores with release, under `gate.m` (G3). The executor loads
 *   with acquire, with the mutex (G2) or without it (G1).
 * - Linearization point: the writer's store (G3).
 * - Progress: a writer takes a short lock. The executor blocks, bounded by the deadline of
 *   the participant it waits for.
 * - Failure: a deadline that passes applies the participant's expiry policy.
 *
 * Tick progress
 * - Atomic objects: `first_unexecuted` (8 bytes), always lock-free on x86-64.
 * - Memory orders: the executor stores with release, under `gate.m` (G5). A waiter loads
 *   relaxed, under the mutex.
 * - Linearization point: the executor's store (G5).
 * - Progress: the executor takes a short lock per tick. A waiter blocks until the tick has
 *   run.
 * - Failure: none. A waiter that reaches its `until` is told the tick has not run.
 *
 * Both follow the wake-up rule: a predicate a parked thread waits on changes only under
 * `gate.m`, followed by a notify. Nothing waits, logs or calls out while holding `gate.m`.
 *
 * This partition is internal: an importer of sim_estab cannot see it. Its tests are units
 * of the module (design_patterns.md §10).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>

// Module declaration
module sim_estab:engine.gate;

import :types;

/**
 * @namespace sim_estab::core::engine::gate
 * @brief The gate and the tick progress wait
 */
namespace sim_estab::core::engine::gate {

/// The expiry policy: what happens when a participant's deadline passes
/// (design_engine_core.md §3.3). Drop arrives with network peers and Suspend with mods.
enum class OnExpiry : std::uint32_t {
    Fail,            ///< log critical; the engine enters the terminal failed state
    ContinueWithout, ///< the participant leaves the conjunction for good; ticks go on
};

/// Something the gate waits for before each tick, up to its deadline
struct Participant {
    std::atomic<std::uint64_t> ready_through;   ///< monotone; UINT64_MAX = unconditionally ready
    std::atomic<std::uint32_t> active;          ///< 0 removes it from the conjunction
    std::uint32_t deadline_ms = 0;              ///< declared at registration
    OnExpiry on_expiry        = OnExpiry::Fail; ///< declared at registration
};

/// The host's control of whether ticks run. The host is not a participant.
struct HostControl {
    std::atomic<std::uint64_t> run_until{UINT64_MAX}; ///< exclusive tick ceiling; UINT64_MAX = resumed
    std::atomic<std::uint32_t> stop_requested{0};     ///< sticky: 0 -> 1 only
    bool step_in_flight = false;                      ///< guarded by gate.m; never read by the executor
};

/// The kinds of thing that block a tick, in priority order
enum class BlockerType : std::uint32_t { NONE, STOP, EVENT_BACKLOG, HOST_PAUSE, PARTICIPANT };

/// What blocks a tick: a type, and for PARTICIPANT which one
struct Blocker {
    BlockerType type          = BlockerType::NONE;
    std::uint32_t participant = 0; ///< index into participants[]; 0 unless type is PARTICIPANT

    bool operator==(const Blocker&) const = default;
};

/// The gate mutex and the two parks on it
struct Gate {
    std::mutex m;                        ///< held for loads, stores and notifies only
    std::condition_variable sim_cv;      ///< the executor parks here, and nothing else does
    std::condition_variable progress_cv; ///< threads waiting for a tick to run park here
    Blocker parked;                      ///< why the executor is parked; NONE while it runs
    std::uint32_t progress_waiters = 0;
};

/// What pass() tells the executor about a tick
struct PassResult {
    Blocker blocker;      ///< what still blocks the tick; NONE: the tick may run
    bool expired = false; ///< the blocker is a participant whose deadline passed under OnExpiry::Fail
};

/// Why begin_step() refused
enum class StepRefusal : std::uint32_t {
    StepInFlight, ///< another step holds the grant
    Overflow,     ///< the grant's ceiling does not fit a tick
};

/**
 * The whole gate (design_engine_core.md §3.3)
 *
 * Three roles use it. The executor runs ticks: it calls blocker_for(), pass() and
 * finish_tick(), from one thread at a time. A writer changes a gate input: a participant
 * declares ready, and a control call pauses, resumes, stops or grants a step. A waiter
 * waits for a tick to run. A field marked executor-private is touched by the executor only.
 */
struct TickGate {
    using Clock = std::chrono::steady_clock;

    std::unique_ptr<Participant[]> participants; ///< fixed at the freeze
    std::uint32_t participant_count;
    HostControl host;
    Gate gate;
    std::atomic<std::uint64_t> first_unexecuted; ///< executor-written; the next tick not yet run
    std::uint64_t backlog    = 0;                ///< executor-private: the event backlog (design_engine_core.md §5.2)
    std::uint64_t high_water = UINT64_MAX;       ///< the backlog at which the gate blocks; fixed at the freeze

    /// Executor-private: when the wait for the tick at the gate began. Every participant's
    /// deadline for that tick counts from it. Empty while no deadline is running.
    std::optional<Clock::time_point> wait_started;

    /**
     * A gate for `count` participants, whose next tick to run is `first_tick`
     *
     * The tick before `first_tick` is already published (design_engine_core.md §2.4 step 9),
     * and every participant starts ready through it. So `first_tick` is at least 1. The run
     * grant starts with no ceiling. The creator sets each participant's `deadline_ms` and
     * `on_expiry` before another thread uses the gate.
     */
    TickGate(std::uint32_t count, types::Tick first_tick)
        : participants(std::make_unique<Participant[]>(count)), participant_count(count),
          first_unexecuted(first_tick.value) {
        for (std::uint32_t i = 0; i < participant_count; ++i) {
            participants[i].ready_through.store(first_tick.value - 1, std::memory_order_relaxed);
            participants[i].active.store(1, std::memory_order_relaxed);
        }
    }

    TickGate(const TickGate&)            = delete;
    TickGate& operator=(const TickGate&) = delete;

    /// Executor: what blocks `tick`, in priority order. NONE if nothing does.
    [[nodiscard]] Blocker blocker_for(types::Tick tick) const noexcept {
        if (host.stop_requested.load(std::memory_order_acquire) != 0) {
            return {BlockerType::STOP};
        }
        if (backlog >= high_water) {
            return {BlockerType::EVENT_BACKLOG};
        }
        if (host.run_until.load(std::memory_order_acquire) <= tick.value) {
            return {BlockerType::HOST_PAUSE};
        }
        for (std::uint32_t i = 0; i < participant_count; ++i) {
            const Participant& participant = participants[i];
            if (participant.active.load(std::memory_order_acquire) != 0 &&
                participant.ready_through.load(std::memory_order_acquire) < tick.value) {
                return {BlockerType::PARTICIPANT, i};
            }
        }
        return {};
    }

    /**
     * Executor: wait at the gate until `tick` may run, or until the executor must act
     *
     * It parks only while a participant blocks the tick, and at most until `until`. So a
     * caller that passes a near `until` waits in slices, and may check for an interrupt
     * between them. When a participant's deadline passes, its expiry policy applies.
     *
     * @return the blocker still in the way. NONE: run the tick. STOP, EVENT_BACKLOG or
     *         HOST_PAUSE: the step ends. PARTICIPANT with `expired`: its deadline passed
     *         under OnExpiry::Fail. PARTICIPANT without it: `until` passed.
     */
    [[nodiscard]] PassResult pass(types::Tick tick, Clock::time_point until);

    /// Executor: `tick` has run and is published. Wakes the threads waiting for it. (G5)
    void finish_tick(types::Tick tick) {
        std::lock_guard lk(gate.m);
        first_unexecuted.store(tick.value + 1, std::memory_order_release); // (G5) the tick has run
        if (gate.progress_waiters != 0) {
            gate.progress_cv.notify_all();
        }
    }

    /// Writer, one per participant: declare `participant` ready through `tick`. `tick` never
    /// goes back.
    void declare_ready(std::uint32_t participant, types::Tick tick) {
        change(participants[participant].ready_through, tick.value);
    }

    /// Writer, any thread: no tick runs again in this session
    void request_stop() { change(host.stop_requested, 1u); }

    /// Writer, any thread: no tick runs after the one in progress, until a resume or a step
    void pause() {
        std::lock_guard lk(gate.m);
        host.run_until.store(first_unexecuted.load(std::memory_order_relaxed), std::memory_order_release); // (G3)
        notify_parked();                                                                                   // (G4)
    }

    /// Writer, any thread: lift the ceiling on the run grant.
    /// @return false if a step is in flight; nothing changes then
    [[nodiscard]] bool resume() {
        std::lock_guard lk(gate.m);
        if (host.step_in_flight) {
            return false;
        }
        host.run_until.store(UINT64_MAX, std::memory_order_release); // (G3)
        notify_parked();                                             // (G4)
        return true;
    }

    /**
     * Stepping thread: reserve the step and grant `tick_count` ticks
     *
     * @return the first tick of the grant, or why the step is refused
     */
    [[nodiscard]] std::expected<types::Tick, StepRefusal> begin_step(std::uint64_t tick_count) {
        std::lock_guard lk(gate.m);
        if (host.step_in_flight) {
            return std::unexpected(StepRefusal::StepInFlight);
        }
        const std::uint64_t start = first_unexecuted.load(std::memory_order_relaxed);
        if (tick_count >= UINT64_MAX - start) { // a ceiling of UINT64_MAX means resumed
            return std::unexpected(StepRefusal::Overflow);
        }
        host.step_in_flight = true;
        host.run_until.store(start + tick_count, std::memory_order_release); // (G3)
        notify_parked();                                                     // (G4)
        return types::Tick{start};
    }

    /// Stepping thread: end the step begun with begin_step(). What is left of the grant is
    /// revoked, so `run_until` equals `first_unexecuted` afterwards.
    void end_step() {
        std::lock_guard lk(gate.m);
        host.step_in_flight = false;
        host.run_until.store(first_unexecuted.load(std::memory_order_relaxed), std::memory_order_release); // (G3)
        notify_parked();                                                                                   // (G4)
    }

    /**
     * Waiter, any thread: wait until tick `target - 1` has run, at most until `until`
     *
     * @return true if `first_unexecuted` has reached `target`
     */
    [[nodiscard]] bool wait_for_tick(types::Tick target, Clock::time_point until) {
        std::unique_lock lk(gate.m);
        ++gate.progress_waiters;
        const bool ran = gate.progress_cv.wait_until(
            lk, until, [&] { return first_unexecuted.load(std::memory_order_relaxed) >= target.value; });
        --gate.progress_waiters;
        return ran;
    }

    /// Writer: change one gate input. Every store to one goes through here or copies its two
    /// steps.
    template <typename T> void change(std::atomic<T>& input, std::type_identity_t<T> value) {
        std::lock_guard lk(gate.m);
        input.store(value, std::memory_order_release); // (G3) LINEARIZATION POINT
        notify_parked();                               // (G4)
    }

    /// Writer, holding gate.m: wake the executor, and only if it is parked. (G4)
    void notify_parked() {
        if (gate.parked.type != BlockerType::NONE) {
            gate.sim_cv.notify_one();
        }
    }
};

} // namespace sim_estab::core::engine::gate
