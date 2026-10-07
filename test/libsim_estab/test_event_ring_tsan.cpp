/**
 * @file test_event_ring_tsan.cpp
 * @brief Race tests for the event ring
 *
 * An executor thread enqueues, the owner drain empties the ring on another thread, and a
 * third thread raises events outside a tick. These tests prove something only in a
 * ThreadSanitizer build: a wrongly ordered enqueue or drain still delivers correct events on
 * x86-64 (design_engine_core.md §5.2).
 *
 * They cover the forward edge (a drained event is whole), the reverse edge (the executor
 * writes a slot again only after the drain has read it), and the side list's lock.
 *
 * sim_estab:engine.event_ring is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

module sim_estab;
import :engine.event_ring;

using namespace sim_estab::core::engine::event_ring;

namespace {

/// Enough events for the ring below to wrap thousands of times
constexpr std::uint64_t ring_events = 200'000;
constexpr std::uint64_t side_events = 5'000;

/// The executor's event number `i`: every field depends on `i`, so a torn event shows
EngineEvent ring_event(std::uint64_t i) {
    return {i + 1, EngineEventType::ParticipantExpired, static_cast<std::uint32_t>(i), i * 7 + 3};
}

/// The side thread's event number `j`
EngineEvent side_event(std::uint64_t j) {
    return {j + 1, EngineEventType::SessionStopped, static_cast<std::uint32_t>(j), ~j};
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(event_ring_tsan_tests)

/**
 * @brief Every event reaches the owner drain exactly once, whole, and in the order it was
 *        raised by its thread
 */
BOOST_AUTO_TEST_CASE(test_every_event_arrives_once_whole_and_in_order) {
    EventRing ring{16, 4, EventDrain::Owner}; // 80 slots, mark 64
    std::atomic<bool> executor_done{false};
    std::atomic<bool> raiser_done{false};

    std::thread executor([&] {
        for (std::uint64_t i = 0; i < ring_events; ++i) {
            // the gate's part: no tick runs at the mark, so the ring never fills
            while (ring.backlog() >= ring.high_water) {
                std::this_thread::yield();
            }
            ring.enqueue(ring_event(i));
        }
        executor_done.store(true, std::memory_order_release);
    });
    std::thread raiser([&] {
        for (std::uint64_t j = 0; j < side_events; ++j) {
            ring.raise_outside_tick(side_event(j));
        }
        raiser_done.store(true, std::memory_order_release);
    });

    // the owner drain, on this thread, until a drain that began after both threads finished
    std::uint64_t next_ring = 0;
    std::uint64_t next_side = 0;
    std::uint64_t wrong     = 0;
    std::vector<EngineEvent> out;
    for (bool last = false; !last;) {
        last = executor_done.load(std::memory_order_acquire) && raiser_done.load(std::memory_order_acquire);
        out.clear();
        ring.drain(out);
        for (const EngineEvent& event : out) {
            const bool from_ring = event.type == EngineEventType::ParticipantExpired;
            if (!(event == (from_ring ? ring_event(next_ring) : side_event(next_side)))) {
                ++wrong;
            }
            ++(from_ring ? next_ring : next_side);
        }
    }
    executor.join();
    raiser.join();

    BOOST_TEST(next_ring == ring_events);
    BOOST_TEST(next_side == side_events);
    BOOST_TEST(wrong == 0u);
    BOOST_TEST(ring.backlog() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
