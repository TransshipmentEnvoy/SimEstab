/**
 * @file test_tick_progress_stress.cpp
 * @brief Stress test for the tick progress wait
 *
 * Threads wait for a tick to run while the executor runs ticks. A lost wake-up is a hang,
 * not a data race, so ThreadSanitizer does not report it. The test drives the wait against
 * the tick loop many times, and the CTest timeout is the failure
 * (design_engine_core.md §3.3).
 *
 * sim_estab:engine.gate is not exported, and neither is the tick loop, so this file is a unit of
 * the module (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

module sim_estab;
import :engine.event_ring;
import :engine.gate;
import :engine;

using namespace sim_estab::core::engine;
using sim_estab::core::engine::event_ring::EventDrain;
using sim_estab::core::engine::event_ring::EventRing;
using sim_estab::core::engine::gate::TickGate;

namespace {

using Clock = TickGate::Clock;

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

/// An `until` that no test reaches, so a lost wake-up hangs
Clock::time_point far() { return Clock::now() + std::chrono::hours(1); }

bool never_interrupted() { return false; }

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(tick_progress_stress_tests)

/**
 * @brief Every waiter sees every tick run
 *
 * Each waiter waits for one tick after another. The executor finishes a tick only once
 * every waiter has said it is about to wait for it. So each waiter is on its way to park
 * when the tick's store and notify land. Every eighth tick comes late, so the waiters are
 * parked by then.
 */
BOOST_AUTO_TEST_CASE(test_the_executor_wakes_every_waiter) {
    constexpr std::uint32_t waiter_count = 3;
    constexpr std::uint64_t ticks        = 10'000;

    EventRing events{64, 8, EventDrain::Owner}; // nothing here raises an event
    TickGate tick_gate{0, first_tick, events};
    std::array<std::atomic<std::uint64_t>, waiter_count> announced{}; // the target each waiter waits for next

    // each owned by its waiter until it is joined
    std::array<std::uint64_t, waiter_count> gave_up{};

    std::vector<std::thread> waiters;
    for (std::uint32_t i = 0; i < waiter_count; ++i) {
        waiters.emplace_back([&, i] {
            for (std::uint64_t target = first_tick.value + 1; target <= first_tick.value + ticks; ++target) {
                announced[i].store(target, std::memory_order_release);
                if (!tick_gate.wait_for_tick(Tick{target}, far())) {
                    ++gave_up[i];
                }
            }
        });
    }

    const StepResult result = run_step(
        tick_gate, ticks,
        [&](Tick tick) {
            for (const std::atomic<std::uint64_t>& target : announced) {
                while (target.load(std::memory_order_acquire) < tick.value + 1) {
                    std::this_thread::yield();
                }
            }
            if (tick.value % 8 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        },
        never_interrupted);
    for (std::thread& waiter : waiters) {
        waiter.join();
    }

    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST(result.tick_reached.value == ticks);
    for (std::uint32_t i = 0; i < waiter_count; ++i) {
        BOOST_TEST(gave_up[i] == 0u);
    }
    BOOST_TEST(tick_gate.gate.progress_waiters == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
