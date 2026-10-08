module;

// compat
#include "compat.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>

module sim_estab;
import :engine.endpoint;
import :engine.event_ring;
import :engine.gate;

using namespace sim_estab::core::log;
using sim_estab::core::engine::event_ring::EngineEventType;

namespace sim_estab::core::engine::endpoint {

namespace {

[[noreturn]] void abort_with(const char *what) noexcept {
    sim_estab_log("sim_estab.engine.endpoint", severity_level::critical, "programming error: ", what);
    std::abort();
}

/// The ring depth of an endpoint holding `capacity` commands per tick: one tick's worth, or
/// two for a paced endpoint with a margin (design_engine_core.md §5.1)
std::uint64_t depth_for(std::uint32_t capacity, std::uint32_t margin) {
    if (capacity == 0 || capacity > limits::max_endpoint_capacity) {
        abort_with("an endpoint capacity outside 1..max_endpoint_capacity; the freeze validates it first");
    }
    return margin == 0 ? capacity : 2 * std::uint64_t{capacity};
}

std::uint32_t checked_participant(const gate::TickGate& tick_gate, std::uint32_t participant) {
    if (participant >= tick_gate.participant_count) {
        abort_with("a paced endpoint names a participant the gate does not have");
    }
    return participant;
}

/**
 * The outcome of the command at `position`, which the producer has submitted and whose slot
 * is not reused yet
 *
 * Loads only, so it may run under `gate.m`.
 */
CommandOutcome outcome_at(const Endpoint& endpoint, std::uint64_t position) noexcept {
    const CommandRing& ring  = endpoint.ring;
    const std::uint64_t slot = position % ring.depth;
    if (ring.read.load(std::memory_order_acquire) <= position) { // pairs with (S4): not drained yet
        return {CommandOutcomeType::Pending, types::Tick{endpoint.unpaced ? 0 : ring.slot[slot].tick}};
    }
    const types::Tick drained_at = endpoint.outcome_slot[slot].tick; // written before (S4)
    if (endpoint.tick_gate.first_unexecuted.load(std::memory_order_acquire) <= drained_at.value) { // pairs with (G5)
        return {CommandOutcomeType::Pending, drained_at};
    }
    return endpoint.outcome_slot[slot]; // its type was written before (G5)
}

/// Leaves the endpoint lease on every way out of a submit that entered it
class LeaseGuard {
public:
    explicit LeaseGuard(Endpoint& leased) : endpoint(leased) {}
    LeaseGuard(const LeaseGuard&)            = delete;
    LeaseGuard& operator=(const LeaseGuard&) = delete;
    ~LeaseGuard() { endpoint.leave(); }

private:
    Endpoint& endpoint;
};

} // namespace

Endpoint::Endpoint(gate::TickGate& session_gate, std::uint32_t source_id, std::uint32_t per_tick_capacity)
    : ring(depth_for(per_tick_capacity, 0)), outcome_slot(std::make_unique<CommandOutcome[]>(ring.depth)),
      tick_gate(session_gate), source(source_id), capacity(per_tick_capacity), margin(0), unpaced(true), participant(0),
      lease_state(LeaseState::REVOKED), active_submit(0) {}

Endpoint::Endpoint(gate::TickGate& session_gate, std::uint32_t source_id, std::uint32_t per_tick_capacity,
                   std::uint32_t participant_index, std::uint32_t stamp_margin)
    : ring(depth_for(per_tick_capacity, stamp_margin)), outcome_slot(std::make_unique<CommandOutcome[]>(ring.depth)),
      tick_gate(session_gate), source(source_id), capacity(per_tick_capacity), margin(stamp_margin), unpaced(false),
      participant(checked_participant(session_gate, participant_index)), lease_state(LeaseState::REVOKED),
      active_submit(0) {}

void Endpoint::open() {
    if (lease_state.load(std::memory_order_acquire) != LeaseState::REVOKED) {
        abort_with("open() on an endpoint that is not REVOKED; revoke() must return true first");
    }
    if (active_submit.load(std::memory_order_acquire) != 0) {
        abort_with("open() while submits are still running; revoke() must finish first");
    }
    lease_state.store(LeaseState::OPEN, std::memory_order_release); // the ring keeps its entries
}

bool Endpoint::revoke(Clock::time_point until) {
    {
        // Both parks read lease_state, so the change that can wake them happens under both
        // mutexes (design_engine_core.md §3.3). REVOKED and OPEN, stored later, wake nobody.
        std::lock_guard gate_lk(tick_gate.gate.m);
        std::lock_guard lk(m);
        lease_state.exchange(LeaseState::REVOKING, std::memory_order_acq_rel); // LINEARIZATION POINT
        space_cv.notify_all();                                                 // a producer in the admission wait
        tick_gate.gate.progress_cv.notify_all();                               // a producer waiting for an outcome
    }
    // A read-modify-write, not a load. It reads the latest count, so a submit whose lease saw
    // OPEN is counted here, or has itself seen REVOKING (design_engine_core.md §5.1).
    // test/litmus/ checks this with herd7; x86 and ThreadSanitizer cannot.
    std::unique_lock lk(m);
    if (!revoke_cv.wait_until(lk, until, [&] { return active_submit.fetch_add(0, std::memory_order_acq_rel) == 0; })) {
        return false;
    }
    lease_state.store(LeaseState::REVOKED, std::memory_order_release);
    return true;
}

bool Endpoint::try_enter() noexcept {
    if (lease_state.load(std::memory_order_acquire) != LeaseState::OPEN) {
        return false;
    }
    active_submit.fetch_add(1, std::memory_order_acq_rel);
    if (lease_state.load(std::memory_order_acquire) != LeaseState::OPEN) {
        leave();
        return false;
    }
    return true; // LINEARIZATION POINT: the second load saw OPEN
}

void Endpoint::leave() noexcept {
    if (active_submit.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        lease_state.load(std::memory_order_acquire) != LeaseState::OPEN) {
        // The revoker checks the count under m, so this notify cannot fall between its check
        // and its sleep.
        std::lock_guard lk(m);
        revoke_cv.notify_all();
    }
}

std::expected<CommandHandle, Rejection> Endpoint::submit(const Command& command) {
    if (!unpaced) {
        abort_with("submit(command) on a paced endpoint; a paced command names its tick");
    }
    if (!try_enter()) { // (H0)
        return std::unexpected(Rejection::Revoked);
    }
    const LeaseGuard lease{*this};

    const std::uint64_t index = ring.write.load(std::memory_order_relaxed); // our own word
    if (index - ring.read.load(std::memory_order_acquire) == ring.depth) {  // (H1) never waits
        return std::unexpected(Rejection::QueueFull);
    }
    ring.slot[index % ring.depth] = command;                // (H2) plain: the slot is ours
    ring.write.store(index + 1, std::memory_order_release); // (H3) LINEARIZATION POINT
    return CommandHandle{source, index};
}

std::expected<CommandHandle, Rejection> Endpoint::submit(const Command& command, types::Tick tick) {
    if (unpaced) {
        abort_with("submit(command, tick) on an unpaced endpoint; its commands name no tick");
    }
    if (!try_enter()) { // (C0)
        return std::unexpected(Rejection::Revoked);
    }
    const LeaseGuard lease{*this};

    const std::uint64_t next_to_run = tick_gate.first_unexecuted.load(std::memory_order_acquire);
    const std::uint64_t released    = // our own declaration
        tick_gate.participants[participant].ready_through.load(std::memory_order_relaxed);
    if (tick.value < next_to_run || tick.value <= released) { // (C1a) past, or released
        return std::unexpected(Rejection::TooLate);
    }
    if (tick.value < last_stamped) { // (C1b)
        return std::unexpected(Rejection::OutOfOrder);
    }
    if (tick.value > next_to_run + margin) { // (C1c)
        return std::unexpected(Rejection::OverMargin);
    }
    if (tick.value != last_stamped) {
        last_stamped     = tick.value;
        stamped_for_tick = 0;
    }
    if (stamped_for_tick == capacity) { // (C1d)
        return std::unexpected(Rejection::QueueFull);
    }

    const std::uint64_t index = ring.write.load(std::memory_order_relaxed); // (C2) our own word
    if (index - ring.read.load(std::memory_order_acquire) == ring.depth) {  // (C3) THE ADMISSION WAIT
        std::unique_lock lk(m); // re-check under m: the drain stores read under it (S4)
        ++space_waiters;
        space_cv.wait(lk, [&] {
            return index - ring.read.load(std::memory_order_relaxed) < ring.depth ||
                   lease_state.load(std::memory_order_relaxed) != LeaseState::OPEN;
        });
        --space_waiters;
        if (lease_state.load(std::memory_order_relaxed) != LeaseState::OPEN) {
            return std::unexpected(Rejection::Revoked); // m is released before the lease is left
        }
    }
    Command& slot = ring.slot[index % ring.depth]; // (C4) plain: the slot is ours
    slot          = command;
    slot.tick     = tick.value;
    ++stamped_for_tick;
    ring.write.store(index + 1, std::memory_order_release); // (C5) LINEARIZATION POINT
    return CommandHandle{source, index};
}

CommandOutcome Endpoint::outcome(const CommandHandle& handle) const {
    const std::uint64_t written = ring.write.load(std::memory_order_relaxed); // our own word
    if (handle.source != source || handle.position >= written) {
        abort_with("outcome() with a handle this endpoint did not issue");
    }
    if (written - handle.position > ring.depth) {
        // The producer has submitted the command that reuses this slot, and the drain may be
        // writing its outcome there.
        return {CommandOutcomeType::Expired, {}};
    }
    return outcome_at(*this, handle.position);
}

std::expected<CommandOutcome, OutcomeWaitRefusal> Endpoint::wait_outcome(const CommandHandle& handle,
                                                                         Clock::time_point until) {
    const CommandOutcome now = outcome(handle);
    if (now.type != CommandOutcomeType::Pending) {
        return now;
    }
    // The deadlock rule: a paced producer may wait only for a tick it has released
    // (design_engine_core.md §5.1). The slot still holds the tick the command names.
    if (!unpaced && tick_gate.participants[participant].ready_through.load(std::memory_order_relaxed) <
                        ring.slot[handle.position % ring.depth].tick) {
        return std::unexpected(OutcomeWaitRefusal::Unreleased);
    }

    // The tick-progress wait (design_engine_core.md §3.3). Its outcome turns final only at
    // (G5), and revocation changes lease_state, both under gate.m and followed by a notify.
    gate::Gate& progress = tick_gate.gate;
    CommandOutcome then;
    bool revoked = false;
    {
        std::unique_lock lk(progress.m);
        ++progress.progress_waiters;
        progress.progress_cv.wait_until(lk, until, [&] {
            then    = outcome_at(*this, handle.position);
            revoked = lease_state.load(std::memory_order_relaxed) != LeaseState::OPEN;
            return then.type != CommandOutcomeType::Pending || revoked;
        });
        --progress.progress_waiters;
    }
    if (then.type == CommandOutcomeType::Pending && revoked) {
        return std::unexpected(OutcomeWaitRefusal::Revoked);
    }
    return then;
}

void drain_commands(std::span<Endpoint *const> endpoints, types::Tick tick,
                    std::function_ref<void(const Command&, std::uint32_t, std::uint64_t)> emit) {
    std::uint64_t sequence   = 0;
    const Endpoint *previous = nullptr;
    for (Endpoint *const endpoint : endpoints) {
        if (previous != nullptr && endpoint->source <= previous->source) {
            abort_with("drain_commands() over endpoints not in ascending source id");
        }
        if (previous != nullptr && &endpoint->tick_gate != &previous->tick_gate) {
            abort_with("drain_commands() over endpoints of different gates");
        }
        previous = endpoint;

        CommandRing& ring        = endpoint->ring;
        const std::uint64_t end  = ring.write.load(std::memory_order_acquire); // (S1)
        const std::uint64_t from = ring.read.load(std::memory_order_relaxed);  // our own word
        std::uint64_t index      = from;
        std::uint64_t dropped    = 0;
        for (; index != end; ++index) {
            const Command& command    = ring.slot[index % ring.depth];
            const std::uint64_t stamp = endpoint->unpaced ? tick.value : command.tick; // unpaced runs now
            if (stamp > tick.value) {
                break; // (S2) stamped for a later tick: it and everything behind it stay
            }
            CommandOutcome& outcome = endpoint->outcome_slot[index % ring.depth];
            if (stamp < tick.value) { // (S3) NEVER executed late
                outcome = {CommandOutcomeType::Dropped, tick};
                ++dropped;
                continue;
            }
            emit(command, endpoint->source, sequence++);
            // M1: the one command does nothing. From M4 the terminal commit writes the type,
            // before (G5).
            outcome = {CommandOutcomeType::Applied, tick};
        }
        if (dropped != 0) {
            endpoint->tick_gate.events.enqueue(
                {tick.value, EngineEventType::CommandProtocolError, endpoint->source, dropped});
        }
        if (index == from) {
            continue;
        }
        if (endpoint->unpaced) {
            ring.read.store(index, std::memory_order_release); // (S4) nothing parks on an unpaced ring
        } else {
            std::lock_guard lk(endpoint->m);                   // freed space: §3.3's rule
            ring.read.store(index, std::memory_order_release); // (S4)
            if (endpoint->space_waiters != 0) {
                endpoint->space_cv.notify_all(); // (S5) only a parked producer
            }
        }
    }
}

} // namespace sim_estab::core::engine::endpoint
