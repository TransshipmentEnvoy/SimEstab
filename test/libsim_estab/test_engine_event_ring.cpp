/**
 * @file test_engine_event_ring.cpp
 * @brief Tests for the event ring
 *
 * These check the ring's sizes, the order and contents of what a drain delivers, the side
 * list's place in that order, and the sink (design_engine_core.md §5.2). They run on one
 * thread; test_event_ring_tsan.cpp runs the ring across threads.
 *
 * sim_estab:engine.event_ring is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

module sim_estab;
import :engine.event_ring;

using namespace sim_estab::core::engine::event_ring;

namespace {

EngineEvent expired(std::uint64_t tick, std::uint32_t participant) {
    return {tick, EngineEventType::ParticipantExpired, participant};
}

EngineEvent stopped(std::uint64_t tick) { return {tick, EngineEventType::SessionStopped}; }

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_event_ring_tests)

/**
 * @brief E counts every term of design_engine_core.md §5.2
 */
BOOST_AUTO_TEST_CASE(test_events_per_tick_counts_every_term) {
    BOOST_TEST(events_per_tick(0, 0, 0, 0) == 4u);
    // 2 types x 3 sources + 3 sources + 3 x 5 participants + 7 engine views + 4
    BOOST_TEST(events_per_tick(2, 3, 5, 7) == 35u);
    // design_limits.md §2.2: 256 sources and participants, 64 engine views, no object type yet
    BOOST_TEST(events_per_tick(0, 256, 256, 64) == 1'092u);
}

/**
 * @brief A ring holds E x (D + 1) events, and its mark is E x D
 */
BOOST_AUTO_TEST_CASE(test_a_ring_is_sized_from_e_and_d) {
    EventRing ring{10, 8, EventDrain::Owner};

    BOOST_TEST(ring.size == 90u);
    BOOST_TEST(ring.high_water == 80u);
    BOOST_TEST(ring.backlog() == 0u);
    BOOST_TEST(ring.sink_count == 0u);
    BOOST_TEST(ring.side_list.empty());
}

/**
 * @brief A drain delivers the enqueued events whole and in order, and frees their slots
 */
BOOST_AUTO_TEST_CASE(test_a_drain_delivers_events_in_order) {
    EventRing ring{4, 2, EventDrain::Owner};
    const std::vector<EngineEvent> raised{expired(1, 3),
                                          {1, EngineEventType::SimBacklogPaused},
                                          {2, EngineEventType::SimBacklogResumed, 0, 42},
                                          stopped(5)};

    for (const EngineEvent& event : raised) {
        ring.enqueue(event);
    }
    BOOST_TEST(ring.backlog() == 4u);

    std::vector<EngineEvent> out;
    ring.drain(out);
    BOOST_TEST((out == raised));
    BOOST_TEST(ring.backlog() == 0u);

    // a drain with nothing raised appends nothing
    ring.drain(out);
    BOOST_TEST(out.size() == 4u);
}

/**
 * @brief The ring reuses its slots: many more events than it holds pass through it
 */
BOOST_AUTO_TEST_CASE(test_the_ring_wraps_around) {
    EventRing ring{1, 2, EventDrain::Owner}; // 3 slots
    std::vector<EngineEvent> out;

    for (std::uint64_t tick = 1; tick <= 100; ++tick) {
        ring.enqueue(expired(tick, 0));
        ring.enqueue(expired(tick, 1));
        ring.drain(out);
    }

    BOOST_TEST(out.size() == 200u);
    for (std::uint64_t i = 0; i < out.size(); ++i) {
        BOOST_TEST((out[i] == expired(i / 2 + 1, static_cast<std::uint32_t>(i % 2))));
    }
}

/**
 * @brief An event raised outside a tick goes before the ring's events of its tick, and
 *        events of one tick keep the order they were raised in
 */
BOOST_AUTO_TEST_CASE(test_an_outside_event_goes_before_the_ring_events_of_its_tick) {
    EventRing ring{8, 2, EventDrain::Owner};
    ring.enqueue(expired(1, 0));
    ring.enqueue(expired(2, 0));
    ring.enqueue(expired(2, 1));
    ring.enqueue(expired(3, 0));
    ring.raise_outside_tick(stopped(2));
    ring.raise_outside_tick(stopped(1));
    ring.raise_outside_tick(stopped(5));
    ring.raise_outside_tick({2, EngineEventType::SessionStopped, 7});

    std::vector<EngineEvent> out;
    ring.drain(out);

    const std::vector<EngineEvent> expected{
        stopped(1),    expired(1, 0), stopped(2),    {2, EngineEventType::SessionStopped, 7},
        expired(2, 0), expired(2, 1), expired(3, 0), stopped(5)};
    BOOST_TEST((out == expected));
    BOOST_TEST(ring.side_list.empty());
}

/**
 * @brief The sink counts the ring's events and the side list's, and keeps nothing
 */
BOOST_AUTO_TEST_CASE(test_the_sink_counts_and_keeps_nothing) {
    EventRing ring{4, 2, EventDrain::Sink};
    ring.enqueue(expired(1, 0));
    ring.enqueue(expired(1, 1));
    ring.enqueue(stopped(2));
    ring.raise_outside_tick(stopped(1));

    ring.drain_to_sink();
    BOOST_TEST(ring.sink_count == 4u);
    BOOST_TEST(ring.backlog() == 0u);
    BOOST_TEST(ring.side_list.empty());

    ring.drain_to_sink();
    BOOST_TEST(ring.sink_count == 4u);
}

BOOST_AUTO_TEST_SUITE_END()
