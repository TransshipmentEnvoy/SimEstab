/**
 * @file test_revoke_wait_stress.cpp
 * @brief Stress tests for the revoker's wait for running submits
 *
 * revoke() parks on the endpoint's mutex until every running submit has left the lease,
 * and the last one to leave wakes it (design_engine_core.md §5.1). A lost wake-up is a
 * hang, not a data race, so ThreadSanitizer does not report it. The test drives the park
 * many times, and the CTest timeout is the failure.
 *
 * revoke() runs with an `until` no test reaches, so a lost wake-up hangs instead of
 * returning false.
 *
 * The mechanism of sim_estab:engine.endpoint is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <thread>

module sim_estab;
import :engine.endpoint;
import :engine.event_ring;
import :engine.gate;
import :engine.lease;

using namespace sim_estab::core::engine::endpoint;
using sim_estab::core::engine::event_ring::EventDrain;
using sim_estab::core::engine::event_ring::EventRing;
using sim_estab::core::engine::gate::TickGate;
using sim_estab::core::engine::lease::LeaseState;
using sim_estab::core::engine::tick::Tick;

namespace {

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

/// An `until` that no test reaches
Endpoint::Clock::time_point far() { return Endpoint::Clock::now() + std::chrono::hours(1); }

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(revoke_wait_stress_tests)

/**
 * @brief The last submit to leave a closing endpoint wakes the revoker
 *
 * In each round the producer holds the lease for a while, as a long submit would, then
 * submits until it is refused. The revoke lands while the lease is held, or races the
 * submits that follow.
 */
BOOST_AUTO_TEST_CASE(test_the_last_submit_wakes_the_revoker) {
    constexpr std::uint64_t rounds = 5'000;

    EventRing events{64, 8, EventDrain::Owner};
    TickGate tick_gate{0, first_tick, events};
    Endpoint host{tick_gate, 0, 8};

    std::atomic<std::uint64_t> round_opened{0};
    std::atomic<std::uint64_t> round_leased{0}; // the producer holds the lease in this round
    std::atomic<std::uint64_t> round_left{0};

    std::thread producer([&] {
        for (std::uint64_t round = 1; round <= rounds; ++round) {
            while (round_opened.load(std::memory_order_acquire) < round) {
                std::this_thread::yield();
            }
            if (host.lease.try_enter()) {
                round_leased.store(round, std::memory_order_release);
                for (std::uint64_t spin = 0; spin < round % 32; ++spin) {
                    std::this_thread::yield();
                }
                host.lease.leave();
            } else {
                round_leased.store(round, std::memory_order_release);
            }
            while (host.submit(Command{}).error_or(Rejection::QueueFull) != Rejection::Revoked) {
                // QueueFull or admitted: keep submitting until refused
            }
            round_left.store(round, std::memory_order_release);
        }
    });

    for (std::uint64_t round = 1; round <= rounds; ++round) {
        host.open();
        round_opened.store(round, std::memory_order_release);
        if (round % 2 == 0) {
            while (round_leased.load(std::memory_order_acquire) < round) {
                std::this_thread::yield();
            }
        }
        BOOST_REQUIRE(host.revoke(far()));
        while (round_left.load(std::memory_order_acquire) < round) {
            std::this_thread::yield();
        }
        host.ring.read.store(host.ring.write.load()); // empty the ring for the next round
    }
    producer.join();

    BOOST_TEST((host.lease.state.load() == LeaseState::CLOSED));
    BOOST_TEST(host.lease.active.load() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
