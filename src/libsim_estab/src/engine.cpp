module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>

module sim_estab;
import :engine.event_ring;
import :engine.gate;

namespace sim_estab::core::engine {

namespace {

/// Under the sink drain, the executor empties the event ring after every tick and when a step
/// ends (design_engine_core.md §5.2). Under the owner drain the owner thread empties it.
void drain_if_sink(gate::TickGate& tick_gate) {
    if (tick_gate.events.event_drain == event_ring::EventDrain::Sink) {
        tick_gate.events.drain_to_sink();
    }
}

/// Ends the step on every way out of run_step(), a throwing tick included
struct StepGuard {
    gate::TickGate& tick_gate;

    explicit StepGuard(gate::TickGate& g) : tick_gate(g) {}
    StepGuard(const StepGuard&)            = delete;
    StepGuard& operator=(const StepGuard&) = delete;
    ~StepGuard() {
        // Between steps the grant is revoked, which is a pause, and a pause discards the
        // deadline clock (design_engine_core.md §3.3). The next step starts a new one.
        tick_gate.wait_started.reset();
        // The step's last gate check may have raised an event that no later tick drains.
        drain_if_sink(tick_gate);
        tick_gate.end_step();
    }
};

} // namespace

StepResult run_step(gate::TickGate& tick_gate, std::uint64_t tick_count, std::function_ref<void(Tick)> run_tick,
                    std::function_ref<bool()> is_interrupted) {
    using gate::BlockerType;
    using Clock = gate::TickGate::Clock;

    const auto start = tick_gate.begin_step(tick_count);
    if (!start) {
        const StepEnd refusal =
            start.error() == gate::StepRefusal::StepInFlight ? StepEnd::StepInFlight : StepEnd::Overflow;
        return {refusal, Tick{tick_gate.first_unexecuted.load(std::memory_order_acquire) - 1}};
    }
    const StepGuard guard{tick_gate};

    Tick tick                    = *start;
    Clock::time_point next_check = Clock::now() + interrupt_check_slice;
    for (;;) {
        const gate::PassResult passed = tick_gate.pass(tick, next_check); // (G1) to (G4)
        switch (passed.blocker.type) {
        case BlockerType::NONE:
            run_tick(tick);
            drain_if_sink(tick_gate);
            tick_gate.finish_tick(tick); // (G5)
            tick = tick + 1;
            if (Clock::now() < next_check) {
                continue;
            }
            break;
        case BlockerType::STOP:
            return {StepEnd::Stopped, tick - 1};
        case BlockerType::EVENT_BACKLOG:
            return {StepEnd::EventBacklog, tick - 1};
        case BlockerType::HOST_PAUSE:
            return {StepEnd::GrantEnded, tick - 1};
        case BlockerType::PARTICIPANT:
            if (passed.expired) {
                return {StepEnd::Failed, tick - 1, passed.blocker.participant};
            }
            break;
        }

        // A slice has passed, at a tick boundary or while waiting at the gate.
        if (is_interrupted()) {
            return {StepEnd::Interrupted, tick - 1};
        }
        next_check = Clock::now() + interrupt_check_slice;
    }
}

} // namespace sim_estab::core::engine
