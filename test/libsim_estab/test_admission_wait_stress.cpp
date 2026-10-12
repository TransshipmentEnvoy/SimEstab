/**
 * @file test_admission_wait_stress.cpp
 * @brief Stress tests for the admission wait, and for revocation waking parked producers
 *
 * A paced producer whose ring is full parks on its endpoint's mutex until the drain frees
 * space, and revocation wakes it, and a producer waiting for an outcome, early. A lost
 * wake-up is a hang, not a data race, so ThreadSanitizer does not report it. Each test
 * drives the park many times, and the CTest timeout is the failure (design_engine_core.md
 * §3.3, §5.1).
 *
 * Every wait here has an `until` no test reaches, so a lost wake-up hangs.
 *
 * The mechanism of sim_estab:engine.endpoint is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <thread>

module sim_estab;
import :engine.endpoint;
import :engine.event_ring;
import :engine.gate;

using namespace sim_estab::core::engine::endpoint;
using sim_estab::core::engine::event_ring::EventDrain;
using sim_estab::core::engine::event_ring::EventRing;
using sim_estab::core::engine::gate::BlockerType;
using sim_estab::core::engine::gate::TickGate;
using sim_estab::core::engine::tick::Tick;

namespace {

using Clock = Endpoint::Clock;

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

/// A deadline no test reaches, so a lost wake-up hangs instead of expiring
constexpr std::uint32_t one_hour_ms = 3'600'000;

/// An `until` that no test reaches
Clock::time_point far() { return Clock::now() + std::chrono::hours(1); }

/// Every eighth round the executor comes late, so the producer is parked when the drain
/// lands. In the other rounds the drain races the producer to the park.
void wait_if_late_round(std::uint64_t round) {
    if (round % 8 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

/// True while a producer is parked on `endpoint`'s admission wait
bool parked_for_space(Endpoint& endpoint) {
    std::lock_guard lk(endpoint.lease.m);
    return endpoint.space_waiters != 0;
}

/// True while a thread is parked on the tick-progress wait
bool parked_for_progress(TickGate& tick_gate) {
    std::lock_guard lk(tick_gate.gate.m);
    return tick_gate.gate.progress_waiters != 0;
}

/// Drain one endpoint at `tick` and finish the tick, as the executor does
void run_tick(Endpoint& endpoint, Tick tick) {
    std::array<Endpoint *, 1> one{&endpoint};
    drain_commands(one, tick, [](const Command&, std::uint32_t, std::uint64_t) {});
    endpoint.tick_gate.finish_tick(tick);
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(admission_wait_stress_tests)

/**
 * @brief Every drain that frees space wakes a producer parked on its full ring
 *
 * The ring holds two ticks' worth, one command each. The producer submits and declares a
 * tick at a time, so it fills the ring whenever the executor is two ticks behind, and
 * parks until the oldest tick runs.
 */
BOOST_AUTO_TEST_CASE(test_the_drain_wakes_a_full_ring) {
    constexpr std::uint64_t ticks = 10'000;

    EventRing events{64, 8, EventDrain::Owner}; // nothing here raises an event
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = one_hour_ms;
    Endpoint peer{tick_gate, 2, 1, 0, 3}; // depth 2
    peer.open();

    // owned by the producer until it is joined
    std::uint64_t refused = 0;
    std::uint64_t full    = 0; // submits that found the ring full

    std::thread producer([&] {
        for (Tick tick = first_tick; tick < first_tick + ticks; tick = tick + 1) {
            if (peer.ring.write.load() - peer.ring.read.load() == peer.ring.depth) {
                ++full;
            }
            if (!peer.submit(Command{}, tick)) {
                ++refused;
            }
            tick_gate.declare_ready(0, tick);
        }
    });

    std::uint64_t passed = 0;
    for (Tick tick = first_tick; tick < first_tick + ticks; tick = tick + 1) {
        if (tick_gate.pass(tick, far()).blocker.type == BlockerType::NONE) {
            ++passed;
        }
        wait_if_late_round(tick.value);
        run_tick(peer, tick);
    }
    producer.join();

    BOOST_TEST(passed == ticks);
    BOOST_TEST(refused == 0u);
    BOOST_TEST(peer.ring.read.load() == ticks);
    BOOST_TEST_MESSAGE("submits that found the ring full: " << full);
}

/**
 * @brief A pause wakes no parked producer, and the drain after the resume does
 *
 * In each round the producer parks on a full ring. A pause must leave it parked; the next
 * tick, once resumed, frees its space.
 */
BOOST_AUTO_TEST_CASE(test_a_pause_leaves_a_parked_producer_parked) {
    constexpr std::uint64_t rounds = 500;

    EventRing events{64, 8, EventDrain::Owner};
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = one_hour_ms;
    Endpoint peer{tick_gate, 2, 1, 0, 3}; // depth 2
    peer.open();
    std::atomic<std::uint64_t> admitted{0};

    // The producer is two ticks ahead: its submit for tick r + 2 parks while ticks r and
    // r + 1 fill the ring.
    std::thread producer([&] {
        for (Tick tick = first_tick; tick < first_tick + rounds + 2; tick = tick + 1) {
            if (peer.submit(Command{}, tick)) {
                admitted.fetch_add(1, std::memory_order_release);
            }
            tick_gate.declare_ready(0, tick);
        }
    });

    std::uint64_t woken_by_pause = 0;
    for (Tick tick = first_tick; tick < first_tick + rounds; tick = tick + 1) {
        // Ticks up to tick + 1 are admitted, and the submit for tick + 2 is parked. Counting
        // the admitted ones first keeps the last round's waiter, still waking, from passing
        // for the new one.
        const std::uint64_t before = tick.value + 1;
        while (admitted.load(std::memory_order_acquire) < before || !parked_for_space(peer)) {
            std::this_thread::yield();
        }
        tick_gate.pause();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (!parked_for_space(peer) || admitted.load(std::memory_order_acquire) != before) {
            ++woken_by_pause;
        }
        BOOST_REQUIRE(tick_gate.resume());
        BOOST_REQUIRE((tick_gate.pass(tick, far()).blocker.type == BlockerType::NONE));
        run_tick(peer, tick);
    }
    // the last two ticks free the producer's last submits
    for (Tick tick = first_tick + rounds; tick < first_tick + rounds + 2; tick = tick + 1) {
        BOOST_REQUIRE((tick_gate.pass(tick, far()).blocker.type == BlockerType::NONE));
        run_tick(peer, tick);
    }
    producer.join();

    BOOST_TEST(woken_by_pause == 0u);
    BOOST_TEST(admitted.load() == rounds + 2);
}

/**
 * @brief Revocation wakes a producer parked on the admission wait and one waiting for an
 *        outcome, each with Revoked
 *
 * In half the rounds the revoke comes once both are parked. In the other half it races
 * them there.
 */
BOOST_AUTO_TEST_CASE(test_revocation_wakes_parked_producers) {
    constexpr std::uint64_t rounds = 1'000;

    for (std::uint64_t round = 0; round < rounds; ++round) {
        EventRing events{64, 8, EventDrain::Owner};
        TickGate tick_gate{2, first_tick, events};
        Endpoint full_peer{tick_gate, 2, 1, 0, 3}; // depth 2: its third tick parks
        Endpoint waiting_peer{tick_gate, 3, 1, 1, 0};
        full_peer.open();
        waiting_peer.open();

        Rejection full_result             = Rejection::QueueFull;
        OutcomeWaitRefusal waiting_result = OutcomeWaitRefusal::Unreleased;
        std::thread full_producer([&] {
            for (Tick tick = first_tick;; tick = tick + 1) {
                const auto admitted = full_peer.submit(Command{}, tick);
                if (!admitted) {
                    full_result = admitted.error();
                    return;
                }
                tick_gate.declare_ready(0, tick);
            }
        });
        std::thread waiting_producer([&] {
            const auto admitted = waiting_peer.submit(Command{}, first_tick);
            if (!admitted) {
                waiting_result = OutcomeWaitRefusal::Revoked; // revoked before it submitted
                return;
            }
            tick_gate.declare_ready(1, first_tick);
            const auto outcome = waiting_peer.wait_outcome(*admitted, far()); // no tick ever runs
            if (!outcome) {
                waiting_result = outcome.error();
            }
        });

        if (round % 2 == 0) {
            while (!parked_for_space(full_peer) || !parked_for_progress(tick_gate)) {
                std::this_thread::yield();
            }
        } else {
            for (std::uint64_t spin = 0; spin < round % 64; ++spin) {
                std::this_thread::yield();
            }
        }
        BOOST_REQUIRE(full_peer.revoke(far()));
        BOOST_REQUIRE(waiting_peer.revoke(far()));
        full_producer.join();
        waiting_producer.join();

        BOOST_REQUIRE((full_result == Rejection::Revoked));
        BOOST_REQUIRE((waiting_result == OutcomeWaitRefusal::Revoked));
    }
}

BOOST_AUTO_TEST_SUITE_END()
