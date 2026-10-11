/**
 * @file test_endpoint_tsan.cpp
 * @brief Race tests for command endpoints
 *
 * Producers submit and read outcomes on their own threads while the executor drains, and a
 * lifecycle thread revokes an endpoint under a running producer. These tests prove something
 * only in a ThreadSanitizer build: a wrongly ordered submit, drain or outcome read still
 * delivers correct commands on x86-64 (design_engine_core.md §5.1).
 *
 * They cover the forward edge (a drained command is whole), the reverse edge (a producer
 * writes a slot again only after the drain has read it), both edges of an outcome, and the
 * endpoint lease (no submit touches the ring after revoke() returns).
 *
 * The mechanism of sim_estab:engine.endpoint is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <thread>
#include <vector>

module sim_estab;
import :engine.endpoint;
import :engine.event_ring;
import :engine.gate;

using namespace sim_estab::core::engine::endpoint;
using sim_estab::core::engine::event_ring::EventDrain;
using sim_estab::core::engine::event_ring::EventRing;
using sim_estab::core::engine::gate::BlockerType;
using sim_estab::core::engine::gate::TickGate;
using sim_estab::core::types::Tick;

namespace {

using Clock = Endpoint::Clock;

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

/// A deadline no test reaches
constexpr std::uint32_t one_hour_ms = 3'600'000;

/// An `until` that no test reaches
Clock::time_point far() { return Clock::now() + std::chrono::hours(1); }

/// A command whose every payload byte depends on `mark`, so a torn command shows
Command marked(std::uint64_t mark) {
    Command command;
    for (std::size_t i = 0; i < command.payload.size(); ++i) {
        command.payload[i] = static_cast<std::byte>((mark >> (8 * (i % 8))) ^ i);
    }
    return command;
}

/// The mark of a command marked() made; a torn command gives a mark no producer used
std::uint64_t mark_of(const Command& command) {
    std::uint64_t mark = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        mark |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(command.payload[i]) ^ i) << (8 * i);
    }
    return marked(mark).payload == command.payload ? mark : UINT64_MAX;
}

/// The paced producer's mark for its command `index` of `tick`
std::uint64_t paced_mark(Tick tick, std::uint64_t index) { return (tick.value << 8) | index; }

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(endpoint_tsan_tests)

/**
 * @brief Every admitted command reaches the drain exactly once, whole, in submission order,
 *        at its tick, while both producers read their outcomes
 *
 * The host's producer submits unpaced commands and polls their outcomes. A peer's producer
 * submits a few commands for each tick, declares the tick ready, and waits for the outcomes
 * of the tick before. Its ring holds two ticks' worth, so a slow executor parks it on the
 * admission wait, and a handle stays readable for those two ticks. The executor runs ticks
 * as the gate lets it.
 */
BOOST_AUTO_TEST_CASE(test_every_admitted_command_arrives_once_in_order_at_its_tick) {
    constexpr std::uint64_t host_commands  = 20'000;
    constexpr std::uint64_t paced_ticks    = 4'000;
    constexpr std::uint64_t paced_per_tick = 3;

    EventRing events{64, 8, EventDrain::Owner}; // nothing here raises an event
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = one_hour_ms;
    Endpoint host{tick_gate, 0, 8};
    Endpoint peer{tick_gate, 2, 4, 0, 2}; // it may stamp two past a tick still running
    host.open();
    peer.open();

    std::atomic<bool> host_done{false};
    std::atomic<bool> peer_done{false};

    // owned by each producer until it is joined
    std::uint64_t host_wrong_outcomes = 0;
    std::uint64_t peer_wrong_outcomes = 0;

    std::thread host_producer([&] {
        std::vector<CommandHandle> handles;
        for (std::uint64_t i = 0; i < host_commands;) {
            const auto admitted = host.submit(marked(i));
            if (!admitted) {
                std::this_thread::yield(); // QueueFull: the next drain makes room
                continue;
            }
            handles.push_back(*admitted);
            ++i;
            // poll the oldest outcome still readable
            const CommandOutcome outcome = host.outcome(handles[handles.size() > 8 ? handles.size() - 8 : 0]);
            if (outcome.type == CommandOutcomeType::Dropped || outcome.type == CommandOutcomeType::Expired) {
                ++host_wrong_outcomes;
            }
        }
        // the last `depth` handles are still readable: each must end Applied
        for (std::size_t k = handles.size() - 8; k < handles.size(); ++k) {
            CommandOutcome outcome;
            while ((outcome = host.outcome(handles[k])).type == CommandOutcomeType::Pending) {
                std::this_thread::yield();
            }
            if (outcome.type != CommandOutcomeType::Applied || outcome.tick < first_tick) {
                ++host_wrong_outcomes;
            }
        }
        host_done.store(true, std::memory_order_release);
    });

    std::thread peer_producer([&] {
        std::vector<CommandHandle> previous;
        for (Tick tick = first_tick; tick < first_tick + paced_ticks; tick = tick + 1) {
            std::vector<CommandHandle> current;
            for (std::uint64_t k = 0; k < paced_per_tick; ++k) {
                const auto admitted = peer.submit(marked(paced_mark(tick, k)), tick);
                if (admitted) {
                    current.push_back(*admitted);
                } else {
                    ++peer_wrong_outcomes; // the margin and the capacity allow every one
                }
            }
            tick_gate.declare_ready(0, tick);
            for (const CommandHandle& handle : previous) { // a tick already released
                const auto outcome = peer.wait_outcome(handle, far());
                if (!outcome || *outcome != CommandOutcome{CommandOutcomeType::Applied, tick - 1}) {
                    ++peer_wrong_outcomes;
                }
            }
            previous = std::move(current);
        }
        tick_gate.declare_ready(0, Tick{UINT64_MAX}); // the gate no longer waits for it
        peer_done.store(true, std::memory_order_release);
    });

    // The executor, on this thread, until a drain that began after both producers finished
    // and past the peer's last tick
    std::uint64_t next_host = 0;
    std::uint64_t wrong     = 0;
    std::uint64_t peer_seen = 0;
    std::array<Endpoint *, 2> endpoints{&host, &peer};
    for (Tick tick = first_tick;; tick = tick + 1) {
        const bool last = host_done.load(std::memory_order_acquire) && peer_done.load(std::memory_order_acquire) &&
                          tick >= first_tick + paced_ticks;
        if (tick_gate.pass(tick, far()).blocker.type != BlockerType::NONE) {
            ++wrong;
        }
        std::uint64_t expected_sequence = 0;
        std::uint64_t next_paced        = 0;
        drain_commands(endpoints, tick, [&](const Command& command, std::uint32_t source, std::uint64_t sequence) {
            if (sequence != expected_sequence++) {
                ++wrong;
            }
            if (source == 0) {
                if (mark_of(command) != next_host++) {
                    ++wrong;
                }
            } else if (mark_of(command) != paced_mark(tick, next_paced++) || command.tick != tick.value) {
                ++wrong;
            } else {
                ++peer_seen;
            }
        });
        tick_gate.finish_tick(tick);
        if (last) {
            break;
        }
    }
    host_producer.join();
    peer_producer.join();

    BOOST_TEST(next_host == host_commands);
    BOOST_TEST(peer_seen == paced_ticks * paced_per_tick);
    BOOST_TEST(wrong == 0u);
    BOOST_TEST(host_wrong_outcomes == 0u);
    BOOST_TEST(peer_wrong_outcomes == 0u);
}

/**
 * @brief A paced producer writes a slot again only after the drain has read it
 *
 * The producer stamps two ticks ahead and never waits for an outcome, and the executor takes
 * its time between the drain and (G5). So the producer often refills a slot the running
 * tick's drain has just freed. Nothing but the (S4) and (C3) pair orders the drain's read
 * of that slot before the producer's write: a declaration and an outcome wait would both
 * order it through `gate.m`.
 */
BOOST_AUTO_TEST_CASE(test_a_paced_producer_refills_a_slot_only_after_the_drain_read_it) {
    constexpr std::uint64_t ticks    = 20'000;
    constexpr std::uint64_t per_tick = 2;

    EventRing events{64, 8, EventDrain::Owner};
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = one_hour_ms;
    Endpoint peer{tick_gate, 2, per_tick, 0, 3}; // depth 4: two ticks' worth, stamped up to three ahead
    peer.open();

    // owned by the producer until it is joined
    std::uint64_t refused = 0;

    std::thread producer([&] {
        for (Tick tick = first_tick; tick < first_tick + ticks; tick = tick + 1) {
            for (std::uint64_t k = 0; k < per_tick; ++k) {
                if (!peer.submit(marked(paced_mark(tick, k)), tick)) {
                    ++refused;
                }
            }
            tick_gate.declare_ready(0, tick);
        }
    });

    std::uint64_t wrong = 0;
    std::uint64_t seen  = 0;
    std::array<Endpoint *, 1> endpoints{&peer};
    for (Tick tick = first_tick; tick < first_tick + ticks; tick = tick + 1) {
        if (tick_gate.pass(tick, far()).blocker.type != BlockerType::NONE) {
            ++wrong;
        }
        std::uint64_t next = 0;
        drain_commands(endpoints, tick, [&](const Command& command, std::uint32_t, std::uint64_t) {
            if (mark_of(command) != paced_mark(tick, next++) || command.tick != tick.value) {
                ++wrong;
            }
            ++seen;
        });
        for (int work = 0; work < 4; ++work) { // the tick itself
            std::this_thread::yield();
        }
        tick_gate.finish_tick(tick);
    }
    producer.join();

    BOOST_TEST(seen == ticks * per_tick);
    BOOST_TEST(wrong == 0u);
    BOOST_TEST(refused == 0u);
}

/**
 * @brief A revoke under a running producer: what was admitted stays admitted, every later
 *        submit returns Revoked, and nothing touches the ring once revoke() has returned
 *
 * After each revoke this thread empties the ring and overwrites every slot, as a reclaim
 * would. A submit still writing a slot then would be a race ThreadSanitizer reports. The
 * producer reads no outcome: outcome reads take no lease, so a real reclaim also waits for
 * the producer's join (design_engine_core.md §5.1).
 */
BOOST_AUTO_TEST_CASE(test_nothing_touches_the_ring_after_a_revoke) {
    constexpr std::uint64_t rounds = 2'000;

    EventRing events{64, 8, EventDrain::Owner};
    TickGate tick_gate{0, first_tick, events};
    Endpoint host{tick_gate, 0, 8};

    std::atomic<std::uint64_t> round_opened{0};
    std::atomic<std::uint64_t> round_left{0};

    // owned by the producer until it is joined
    std::uint64_t admitted_total = 0;
    std::uint64_t wrong          = 0;

    std::thread producer([&] {
        for (std::uint64_t round = 1; round <= rounds; ++round) {
            while (round_opened.load(std::memory_order_acquire) < round) {
                std::this_thread::yield();
            }
            for (std::uint64_t i = 0;; ++i) {
                const auto result = host.submit(marked(i));
                if (result) {
                    ++admitted_total;
                } else if (result.error() == Rejection::Revoked) {
                    break;
                } else if (result.error() != Rejection::QueueFull) {
                    ++wrong;
                }
            }
            // once refused, always refused until the next open
            if (host.submit(marked(0)) != std::unexpected(Rejection::Revoked)) {
                ++wrong;
            }
            round_left.store(round, std::memory_order_release);
        }
    });

    std::uint64_t reclaimed = 0;
    for (std::uint64_t round = 1; round <= rounds; ++round) {
        host.open();
        round_opened.store(round, std::memory_order_release);
        for (std::uint64_t spin = 0; spin < round % 64; ++spin) {
            std::this_thread::yield();
        }
        BOOST_REQUIRE(host.revoke(far()));

        // the reclaim: the ring belongs to this thread now
        const std::uint64_t written = host.ring.write.load(std::memory_order_relaxed);
        reclaimed += written - host.ring.read.load(std::memory_order_relaxed);
        for (std::uint64_t slot = 0; slot < host.ring.depth; ++slot) {
            host.ring.slot[slot] = Command{};
        }
        host.ring.read.store(written, std::memory_order_relaxed);

        while (round_left.load(std::memory_order_acquire) < round) {
            std::this_thread::yield();
        }
        BOOST_TEST(host.ring.write.load(std::memory_order_relaxed) == written); // nothing admitted since
    }
    producer.join();

    BOOST_TEST(reclaimed == admitted_total);
    BOOST_TEST(wrong == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
