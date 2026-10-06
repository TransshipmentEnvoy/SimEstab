module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>

module sim_estab;
import :engine.gate;

namespace sim_estab::core::engine {

namespace {

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
        tick_gate.end_step();
    }
};

} // namespace

StepResult run_step(gate::TickGate& tick_gate, std::uint64_t tick_count, std::function_ref<void(Tick)> run_tick,
                    std::function_ref<bool()> is_interrupted) {
    using gate::BlockerKind;
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
        switch (passed.blocker.kind) {
        case BlockerKind::NONE:
            run_tick(tick);
            tick_gate.finish_tick(tick); // (G5)
            tick = tick + 1;
            if (Clock::now() < next_check) {
                continue;
            }
            break;
        case BlockerKind::STOP:
            return {StepEnd::Stopped, tick - 1};
        case BlockerKind::EVENT_BACKLOG:
            return {StepEnd::EventBacklog, tick - 1};
        case BlockerKind::HOST_PAUSE:
            return {StepEnd::GrantEnded, tick - 1};
        case BlockerKind::PARTICIPANT:
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
