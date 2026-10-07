/**
 * @file test_gate_stress.cpp
 * @brief Stress tests for the gate's park
 *
 * The executor parks at the gate while other threads change the gate's inputs. A lost
 * wake-up is a hang, not a data race, so ThreadSanitizer does not report it. Each test
 * drives the park against one kind of writer many times, and the CTest timeout is the
 * failure (design_engine_core.md §3.3).
 *
 * The executor here calls pass() with an `until` it never reaches. A step's slices would
 * hide a lost wake-up, because the next slice finds the change.
 *
 * sim_estab:engine.gate is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

module sim_estab;
import :engine.event_ring;
import :engine.gate;

using namespace sim_estab::core::engine::gate;
using sim_estab::core::engine::event_ring::EventDrain;
using sim_estab::core::engine::event_ring::EventRing;
using sim_estab::core::types::Tick;

namespace {

using Clock = TickGate::Clock;

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

/// A deadline no test reaches, so a lost wake-up hangs instead of expiring
constexpr std::uint32_t one_hour_ms = 3'600'000;

/// An `until` that no test reaches
Clock::time_point far() { return Clock::now() + std::chrono::hours(1); }

/// Every eighth round the writer comes late, so the executor is parked when the change
/// lands. In the other rounds the writer races the executor to the gate.
void wait_if_late_round(std::uint64_t round) {
    if (round % 8 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(gate_stress_tests)

/**
 * @brief Every tick passes the gate when its participants declare ready one by one
 *
 * Each participant declares for a tick only once the tick before it has run. So the
 * executor is at the gate, or on its way there, when each declaration lands.
 */
BOOST_AUTO_TEST_CASE(test_declarations_wake_the_gate) {
    constexpr std::uint32_t participant_count = 3;
    constexpr std::uint64_t ticks             = 10'000;

    EventRing events{64, 8, EventDrain::Owner}; // nothing here raises an event
    TickGate tick_gate{participant_count, first_tick, events};
    for (std::uint32_t i = 0; i < participant_count; ++i) {
        tick_gate.participants[i].deadline_ms = one_hour_ms;
    }

    std::vector<std::thread> participants;
    for (std::uint32_t i = 0; i < participant_count; ++i) {
        participants.emplace_back([&tick_gate, i] {
            for (Tick tick = first_tick; tick < first_tick + ticks; tick = tick + 1) {
                while (tick_gate.first_unexecuted.load(std::memory_order_acquire) < tick.value) {
                    std::this_thread::yield();
                }
                wait_if_late_round(tick.value + i);
                tick_gate.declare_ready(i, tick);
            }
        });
    }

    std::uint64_t passed = 0;
    for (Tick tick = first_tick; tick < first_tick + ticks; tick = tick + 1) {
        if (tick_gate.pass(tick, far()).blocker.type == BlockerType::NONE) {
            ++passed;
        }
        tick_gate.finish_tick(tick);
    }
    for (std::thread& participant : participants) {
        participant.join();
    }

    BOOST_TEST(passed == ticks);
    BOOST_TEST(tick_gate.first_unexecuted.load() == (first_tick + ticks).value);
}

/**
 * @brief Every pause reaches an executor that waits for a participant
 *
 * The participant never declares. In each round the executor goes to wait for it, and a
 * pause from the controller must bring it back.
 */
BOOST_AUTO_TEST_CASE(test_a_pause_wakes_the_gate) {
    constexpr std::uint64_t rounds = 5'000;

    EventRing events{64, 8, EventDrain::Owner}; // nothing here raises an event
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = one_hour_ms;
    std::atomic<std::uint64_t> returned{0}; // rounds the executor has come back from
    std::atomic<std::uint64_t> resumed{0};  // rounds the controller has resumed after

    // owned by the controller until it is joined
    std::uint64_t refused_resumes = 0;

    std::thread controller([&] {
        for (std::uint64_t round = 1; round <= rounds; ++round) {
            wait_if_late_round(round);
            tick_gate.pause();
            while (returned.load(std::memory_order_acquire) < round) {
                std::this_thread::yield();
            }
            if (!tick_gate.resume()) {
                ++refused_resumes;
            }
            resumed.store(round, std::memory_order_release);
        }
    });

    std::uint64_t paused = 0;
    for (std::uint64_t round = 1; round <= rounds; ++round) {
        if (tick_gate.pass(first_tick, far()).blocker.type == BlockerType::HOST_PAUSE) {
            ++paused;
        }
        returned.store(round, std::memory_order_release);
        while (resumed.load(std::memory_order_acquire) < round) {
            std::this_thread::yield();
        }
    }
    controller.join();

    BOOST_TEST(paused == rounds);
    BOOST_TEST(refused_resumes == 0u);
}

/**
 * @brief Every stop reaches an executor that waits for a participant
 *
 * A stop is sticky, so each round has a gate of its own.
 */
BOOST_AUTO_TEST_CASE(test_a_stop_wakes_the_gate) {
    constexpr std::uint64_t rounds = 2'000;

    EventRing events{rounds, 1, EventDrain::Owner}; // each round's stop raises one event here
    std::vector<std::unique_ptr<TickGate>> gates;
    for (std::uint64_t round = 0; round < rounds; ++round) {
        gates.push_back(std::make_unique<TickGate>(1u, first_tick, events));
        gates.back()->participants[0].deadline_ms = one_hour_ms;
    }
    std::atomic<std::uint64_t> entering{0}; // the round whose gate the executor is entering

    std::thread controller([&] {
        for (std::uint64_t round = 1; round <= rounds; ++round) {
            while (entering.load(std::memory_order_acquire) < round) {
                std::this_thread::yield();
            }
            wait_if_late_round(round);
            gates[round - 1]->request_stop();
        }
    });

    std::uint64_t stopped = 0;
    for (std::uint64_t round = 1; round <= rounds; ++round) {
        entering.store(round, std::memory_order_release);
        if (gates[round - 1]->pass(first_tick, far()).blocker.type == BlockerType::STOP) {
            ++stopped;
        }
    }
    controller.join();

    BOOST_TEST(stopped == rounds);
}

BOOST_AUTO_TEST_SUITE_END()
