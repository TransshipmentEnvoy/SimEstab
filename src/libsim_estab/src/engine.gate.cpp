module;

// compat
#include "compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

module sim_estab;
import :engine.gate;

using namespace sim_estab::core::log;

namespace sim_estab::core::engine::gate {

PassResult TickGate::pass(types::Tick tick, Clock::time_point until) {
    for (;;) {
        const Blocker blocker = blocker_for(tick); // (G1) no lock: acquire loads
        switch (blocker.type) {
        case BlockerType::NONE:
            wait_started.reset();
            return {};
        case BlockerType::STOP:
            return {blocker};
        case BlockerType::EVENT_BACKLOG:
        case BlockerType::HOST_PAUSE:
            // A pause stops the deadline clock: nobody in a paused session can be late. The
            // first participant to block after it starts a new one.
            wait_started.reset();
            return {blocker};
        case BlockerType::PARTICIPANT:
            break;
        }

        // Deadlines are absolute and share one start: the first participant to block this
        // tick starts the clock for all of them.
        if (!wait_started) {
            wait_started = Clock::now();
        }
        const Participant& participant   = participants[blocker.participant];
        const Clock::time_point deadline = *wait_started + std::chrono::milliseconds(participant.deadline_ms);

        bool woke = false;
        {
            std::unique_lock lk(gate.m);
            if (blocker_for(tick) != blocker) { // (G2) re-check under the mutex
                continue;
            }
            gate.parked = blocker;
            woke        = gate.sim_cv.wait_until(lk, std::min(deadline, until), [&] {
                return blocker_for(tick) != blocker;
            }); // THE ONE WAIT; releases gate.m
            gate.parked = Blocker{};
        }
        if (woke) {
            continue;
        }
        if (until < deadline) {
            return {blocker}; // the caller's slice ended; the deadline has not
        }

        // The deadline passed.
        switch (participant.on_expiry) {
        case OnExpiry::Fail:
            sim_estab_log("sim_estab.engine.gate", severity_level::critical, "participant ", blocker.participant,
                          " was not ready for tick ", tick.value, " within its deadline of ", participant.deadline_ms,
                          " ms");
            return {blocker, true};
        case OnExpiry::ContinueWithout:
            // Later declarations cannot bring it back: nothing sets `active` again.
            change(participants[blocker.participant].active, 0u);
            sim_estab_log("sim_estab.engine.gate", severity_level::warning, "participant ", blocker.participant,
                          " was not ready for tick ", tick.value, " within its deadline of ", participant.deadline_ms,
                          " ms; the engine continues without it");
            break;
        }
    }
}

} // namespace sim_estab::core::engine::gate
