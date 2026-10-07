/**
 * @file test_engine_gate.cpp
 * @brief Tests for the gate
 *
 * These check what each call does: which blocker a tick meets, what a control call changes,
 * what pass() returns, when a deadline expires, and which events the gate raises
 * (design_engine_core.md §3.3, §5.2). They cannot find a lost wake-up. The stress tests in
 * test_gate_stress.cpp and test_tick_progress_stress.cpp do that.
 *
 * sim_estab:engine.gate is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <thread>
#include <vector>

module sim_estab;
import :engine.event_ring;
import :engine.gate;

using namespace sim_estab::core::engine::gate;
using namespace sim_estab::core::engine::event_ring;
using sim_estab::core::types::Tick;
using namespace std::chrono_literals;

namespace {

using Clock = TickGate::Clock;

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

/// An `until` that no test reaches
Clock::time_point far() { return Clock::now() + 1h; }

bool is_type(const Blocker& blocker, BlockerType type) { return blocker.type == type; }

bool is_participant(const Blocker& blocker, std::uint32_t index) {
    return blocker == Blocker{BlockerType::PARTICIPANT, index};
}

/// An event ring that no test fills unless it means to
EventRing roomy_ring() { return EventRing{64, 8, EventDrain::Owner}; }

/// An event ring whose backlog mark is 4 events
EventRing small_ring() { return EventRing{4, 1, EventDrain::Owner}; }

/// Fill `ring` up to its backlog mark, with events the gate never raises itself
void fill_to_the_mark(EventRing& ring) {
    while (ring.backlog() < ring.high_water) {
        ring.enqueue({0, EngineEventType::ParticipantExpired, 99});
    }
}

/// Everything the gate has raised so far
std::vector<EngineEvent> drained(EventRing& ring) {
    std::vector<EngineEvent> out;
    ring.drain(out);
    return out;
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_gate_tests)

/**
 * @brief A new gate has no ceiling on its grant, and its participants are ready through the
 *        tick the freeze published
 */
BOOST_AUTO_TEST_CASE(test_a_new_gate_is_resumed_and_ready_through_the_published_tick) {
    EventRing events = roomy_ring();
    TickGate tick_gate{2, first_tick, events};

    BOOST_TEST(tick_gate.participant_count == 2u);
    BOOST_TEST(tick_gate.first_unexecuted.load() == first_tick.value);
    BOOST_TEST(tick_gate.host.run_until.load() == UINT64_MAX);
    BOOST_TEST(tick_gate.host.stop_requested.load() == 0u);
    BOOST_TEST(!tick_gate.host.step_in_flight);
    BOOST_TEST(is_type(tick_gate.gate.parked, BlockerType::NONE));
    BOOST_TEST(tick_gate.gate.progress_waiters == 0u);
    for (std::uint32_t i = 0; i < 2; ++i) {
        BOOST_TEST(tick_gate.participants[i].ready_through.load() == first_tick.value - 1);
        BOOST_TEST(tick_gate.participants[i].active.load() == 1u);
    }
}

/**
 * @brief With no participant, nothing blocks a tick
 */
BOOST_AUTO_TEST_CASE(test_nothing_blocks_a_gate_without_participants) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    BOOST_TEST(is_type(tick_gate.blocker_for(first_tick), BlockerType::NONE));
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1'000'000}), BlockerType::NONE));
}

/**
 * @brief A participant blocks every tick past the one it is ready through
 */
BOOST_AUTO_TEST_CASE(test_a_participant_blocks_until_it_declares_ready) {
    EventRing events = roomy_ring();
    TickGate tick_gate{1, first_tick, events};

    BOOST_TEST(is_participant(tick_gate.blocker_for(Tick{1}), 0));

    tick_gate.declare_ready(0, Tick{1});
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1}), BlockerType::NONE));
    BOOST_TEST(is_participant(tick_gate.blocker_for(Tick{2}), 0));

    tick_gate.declare_ready(0, Tick{UINT64_MAX});
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1'000'000}), BlockerType::NONE));
}

/**
 * @brief The blocker is the first active participant that is not ready
 */
BOOST_AUTO_TEST_CASE(test_the_first_participant_that_is_not_ready_blocks) {
    EventRing events = roomy_ring();
    TickGate tick_gate{3, first_tick, events};

    BOOST_TEST(is_participant(tick_gate.blocker_for(Tick{1}), 0));

    tick_gate.declare_ready(0, Tick{1});
    BOOST_TEST(is_participant(tick_gate.blocker_for(Tick{1}), 1));

    tick_gate.change(tick_gate.participants[1].active, 0u);
    BOOST_TEST(is_participant(tick_gate.blocker_for(Tick{1}), 2));

    tick_gate.declare_ready(2, Tick{1});
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1}), BlockerType::NONE));
}

/**
 * @brief A stop comes before the backlog, the backlog before a pause, a pause before a
 *        participant
 */
BOOST_AUTO_TEST_CASE(test_blockers_come_in_priority_order) {
    EventRing events = small_ring();
    TickGate tick_gate{1, first_tick, events};

    BOOST_TEST(is_participant(tick_gate.blocker_for(Tick{1}), 0));

    tick_gate.pause();
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1}), BlockerType::HOST_PAUSE));

    fill_to_the_mark(events);
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1}), BlockerType::EVENT_BACKLOG));

    tick_gate.request_stop();
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1}), BlockerType::STOP));
}

/**
 * @brief Nothing clears a stop
 */
BOOST_AUTO_TEST_CASE(test_a_stop_is_sticky) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    tick_gate.request_stop();
    BOOST_TEST(tick_gate.resume());
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1}), BlockerType::STOP));

    BOOST_TEST(tick_gate.begin_step(3).has_value());
    BOOST_TEST(is_type(tick_gate.blocker_for(Tick{1}), BlockerType::STOP));
    tick_gate.end_step();
    BOOST_TEST(tick_gate.host.stop_requested.load() == 1u);
}

/**
 * @brief A pause holds the next tick, and a resume releases it
 */
BOOST_AUTO_TEST_CASE(test_a_pause_holds_the_next_tick_and_a_resume_releases_it) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    tick_gate.pause();
    BOOST_TEST(tick_gate.host.run_until.load() == first_tick.value);
    BOOST_TEST(is_type(tick_gate.blocker_for(first_tick), BlockerType::HOST_PAUSE));

    BOOST_TEST(tick_gate.resume());
    BOOST_TEST(tick_gate.host.run_until.load() == UINT64_MAX);
    BOOST_TEST(is_type(tick_gate.blocker_for(first_tick), BlockerType::NONE));
}

/**
 * @brief A step's grant covers its ticks and no more, and ending the step revokes the rest
 */
BOOST_AUTO_TEST_CASE(test_a_step_grant_covers_its_ticks) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    const auto start = tick_gate.begin_step(3);
    BOOST_TEST(start.has_value());
    BOOST_TEST((*start == first_tick));
    BOOST_TEST(tick_gate.host.step_in_flight);
    BOOST_TEST(tick_gate.host.run_until.load() == (first_tick + 3).value);
    BOOST_TEST(is_type(tick_gate.blocker_for(first_tick + 2), BlockerType::NONE));
    BOOST_TEST(is_type(tick_gate.blocker_for(first_tick + 3), BlockerType::HOST_PAUSE));

    tick_gate.finish_tick(first_tick);
    tick_gate.end_step();
    BOOST_TEST(!tick_gate.host.step_in_flight);
    BOOST_TEST(tick_gate.host.run_until.load() == (first_tick + 1).value);
    BOOST_TEST(tick_gate.host.run_until.load() == tick_gate.first_unexecuted.load());
}

/**
 * @brief While a step is in flight, a second step and a resume are refused
 */
BOOST_AUTO_TEST_CASE(test_a_step_in_flight_refuses_a_second_step_and_a_resume) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    BOOST_TEST(tick_gate.begin_step(3).has_value());

    const auto second = tick_gate.begin_step(1);
    BOOST_TEST(!second.has_value());
    BOOST_TEST((second.error() == StepRefusal::StepInFlight));

    BOOST_TEST(!tick_gate.resume());
    BOOST_TEST(tick_gate.host.run_until.load() == (first_tick + 3).value);

    tick_gate.end_step();
    BOOST_TEST(tick_gate.begin_step(1).has_value());
    tick_gate.end_step();
    BOOST_TEST(tick_gate.resume());
}

/**
 * @brief A grant whose ceiling does not fit a tick is refused, and changes nothing
 */
BOOST_AUTO_TEST_CASE(test_a_step_that_does_not_fit_is_refused) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    const auto refused = tick_gate.begin_step(UINT64_MAX - first_tick.value);
    BOOST_TEST(!refused.has_value());
    BOOST_TEST((refused.error() == StepRefusal::Overflow));
    BOOST_TEST(!tick_gate.host.step_in_flight);
    BOOST_TEST(tick_gate.host.run_until.load() == UINT64_MAX);

    // the largest grant: its ceiling is one below "resumed"
    BOOST_TEST(tick_gate.begin_step(UINT64_MAX - first_tick.value - 1).has_value());
    BOOST_TEST(tick_gate.host.run_until.load() == UINT64_MAX - 1);
    tick_gate.end_step();
}

/**
 * @brief A tick that has run moves first_unexecuted past it
 */
BOOST_AUTO_TEST_CASE(test_a_finished_tick_moves_first_unexecuted) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    tick_gate.finish_tick(first_tick);
    BOOST_TEST(tick_gate.first_unexecuted.load() == (first_tick + 1).value);
}

/**
 * @brief pass() does not wait at a stop, a pause or the backlog mark, with a participant
 *        blocking too
 */
BOOST_AUTO_TEST_CASE(test_pass_returns_at_once_where_a_step_ends) {
    EventRing events = small_ring();
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = 3'600'000;

    tick_gate.pause();
    const PassResult paused = tick_gate.pass(first_tick, far());
    BOOST_TEST(is_type(paused.blocker, BlockerType::HOST_PAUSE));
    BOOST_TEST(!paused.expired);

    fill_to_the_mark(events);
    const PassResult backlog = tick_gate.pass(first_tick, far());
    BOOST_TEST(is_type(backlog.blocker, BlockerType::EVENT_BACKLOG));

    tick_gate.request_stop();
    const PassResult stopped = tick_gate.pass(first_tick, far());
    BOOST_TEST(is_type(stopped.blocker, BlockerType::STOP));
}

/**
 * @brief The gate raises sim.backlog_paused once per stop at the mark, sim.backlog_resumed
 *        when a tick passes after it, and session.stopped once, each stamped with the tick it
 *        checks
 */
BOOST_AUTO_TEST_CASE(test_the_gate_raises_each_event_once) {
    EventRing events = small_ring();
    TickGate tick_gate{0, first_tick, events};

    fill_to_the_mark(events);
    BOOST_TEST(is_type(tick_gate.pass(first_tick, far()).blocker, BlockerType::EVENT_BACKLOG));
    BOOST_TEST(is_type(tick_gate.pass(first_tick, far()).blocker, BlockerType::EVENT_BACKLOG));
    const std::vector<EngineEvent> at_the_mark = drained(events);
    BOOST_TEST(at_the_mark.size() == 5u); // the four that filled the ring, and one pause
    BOOST_TEST((at_the_mark.back() == EngineEvent{first_tick.value, EngineEventType::SimBacklogPaused}));

    // drained: the next tick passes, and the resume is raised once
    BOOST_TEST(is_type(tick_gate.pass(first_tick, far()).blocker, BlockerType::NONE));
    tick_gate.finish_tick(first_tick);
    BOOST_TEST(is_type(tick_gate.pass(first_tick + 1, far()).blocker, BlockerType::NONE));
    BOOST_TEST((drained(events) == std::vector<EngineEvent>{{first_tick.value, EngineEventType::SimBacklogResumed}}));

    tick_gate.request_stop();
    BOOST_TEST(is_type(tick_gate.pass(first_tick + 1, far()).blocker, BlockerType::STOP));
    BOOST_TEST(is_type(tick_gate.pass(first_tick + 1, far()).blocker, BlockerType::STOP));
    BOOST_TEST(
        (drained(events) == std::vector<EngineEvent>{{(first_tick + 1).value, EngineEventType::SessionStopped}}));
}

/**
 * @brief pass() returns when the caller's slice ends, with the participant still blocking
 *        and its deadline still running
 */
BOOST_AUTO_TEST_CASE(test_pass_returns_when_its_slice_ends) {
    EventRing events = roomy_ring();
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = 3'600'000;

    const Clock::time_point before = Clock::now();
    const PassResult waiting       = tick_gate.pass(first_tick, before + 30ms);

    BOOST_TEST(is_participant(waiting.blocker, 0));
    BOOST_TEST(!waiting.expired);
    BOOST_TEST((Clock::now() - before >= 30ms));
    BOOST_TEST(tick_gate.wait_started.has_value());
    BOOST_TEST(is_type(tick_gate.gate.parked, BlockerType::NONE));

    // once the tick may run, its deadline clock is gone
    tick_gate.declare_ready(0, first_tick);
    BOOST_TEST(is_type(tick_gate.pass(first_tick, far()).blocker, BlockerType::NONE));
    BOOST_TEST(!tick_gate.wait_started.has_value());
}

/**
 * @brief Under Fail, a deadline that passes is reported and the participant stays in place
 */
BOOST_AUTO_TEST_CASE(test_a_deadline_under_fail_expires) {
    EventRing events = roomy_ring();
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = 30;
    tick_gate.participants[0].on_expiry   = OnExpiry::Fail;

    const Clock::time_point before = Clock::now();
    const PassResult failed        = tick_gate.pass(first_tick, far());

    BOOST_TEST(is_participant(failed.blocker, 0));
    BOOST_TEST(failed.expired);
    BOOST_TEST((Clock::now() - before >= 30ms));
    BOOST_TEST(tick_gate.participants[0].active.load() == 1u);
    BOOST_TEST(
        (drained(events) == std::vector<EngineEvent>{{first_tick.value, EngineEventType::ParticipantExpired, 0}}));
}

/**
 * @brief Under ContinueWithout, a deadline that passes takes the participant out for good
 */
BOOST_AUTO_TEST_CASE(test_a_deadline_under_continue_without_takes_the_participant_out) {
    EventRing events = roomy_ring();
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = 30;
    tick_gate.participants[0].on_expiry   = OnExpiry::ContinueWithout;

    const Clock::time_point before = Clock::now();
    const PassResult passed        = tick_gate.pass(first_tick, far());

    BOOST_TEST(is_type(passed.blocker, BlockerType::NONE));
    BOOST_TEST(!passed.expired);
    BOOST_TEST((Clock::now() - before >= 30ms));
    BOOST_TEST(tick_gate.participants[0].active.load() == 0u);
    BOOST_TEST(
        (drained(events) == std::vector<EngineEvent>{{first_tick.value, EngineEventType::ParticipantExpired, 0}}));

    // a late declaration does not bring it back, and it blocks no later tick
    tick_gate.declare_ready(0, first_tick);
    BOOST_TEST(tick_gate.participants[0].active.load() == 0u);
    BOOST_TEST(is_type(tick_gate.blocker_for(first_tick + 100), BlockerType::NONE));
}

/**
 * @brief The deadlines of one tick share one start, so the wait is the largest deadline and
 *        not the sum
 */
BOOST_AUTO_TEST_CASE(test_the_deadlines_of_one_tick_share_one_start) {
    EventRing events = roomy_ring();
    TickGate tick_gate{3, first_tick, events};
    for (std::uint32_t i = 0; i < 3; ++i) {
        tick_gate.participants[i].deadline_ms = 100;
        tick_gate.participants[i].on_expiry   = OnExpiry::ContinueWithout;
    }

    const Clock::time_point before = Clock::now();
    const PassResult passed        = tick_gate.pass(first_tick, far());
    const Clock::duration waited   = Clock::now() - before;

    BOOST_TEST(is_type(passed.blocker, BlockerType::NONE));
    BOOST_TEST((waited >= 100ms));
    BOOST_TEST((waited < 250ms)); // three deadlines one after another would take 300 ms
    for (std::uint32_t i = 0; i < 3; ++i) {
        BOOST_TEST(tick_gate.participants[i].active.load() == 0u);
    }
    const std::vector<EngineEvent> expired{{first_tick.value, EngineEventType::ParticipantExpired, 0},
                                           {first_tick.value, EngineEventType::ParticipantExpired, 1},
                                           {first_tick.value, EngineEventType::ParticipantExpired, 2}};
    BOOST_TEST((drained(events) == expired));
}

/**
 * @brief A pause stops the deadline clock: the participant gets its whole deadline again
 *        after it
 */
BOOST_AUTO_TEST_CASE(test_a_pause_discards_the_deadline_clock) {
    EventRing events = roomy_ring();
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = 200;
    tick_gate.participants[0].on_expiry   = OnExpiry::Fail;

    const PassResult waiting = tick_gate.pass(first_tick, Clock::now() + 20ms);
    BOOST_TEST(is_participant(waiting.blocker, 0));
    BOOST_TEST(!waiting.expired);
    BOOST_TEST(tick_gate.wait_started.has_value());

    tick_gate.pause();
    BOOST_TEST(is_type(tick_gate.pass(first_tick, far()).blocker, BlockerType::HOST_PAUSE));
    BOOST_TEST(!tick_gate.wait_started.has_value());

    std::this_thread::sleep_for(250ms); // longer than the deadline, all of it paused
    BOOST_TEST(tick_gate.resume());

    const PassResult after = tick_gate.pass(first_tick, Clock::now() + 20ms);
    BOOST_TEST(is_participant(after.blocker, 0));
    BOOST_TEST(!after.expired);
}

/**
 * @brief A declaration from another thread lets a parked executor through
 */
BOOST_AUTO_TEST_CASE(test_a_declaration_wakes_a_parked_executor) {
    EventRing events = roomy_ring();
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = 3'600'000;

    std::thread participant([&] {
        std::this_thread::sleep_for(30ms);
        tick_gate.declare_ready(0, first_tick);
    });
    const PassResult passed = tick_gate.pass(first_tick, far());
    participant.join();

    BOOST_TEST(is_type(passed.blocker, BlockerType::NONE));
}

/**
 * @brief A stop from another thread ends the wait for a participant
 */
BOOST_AUTO_TEST_CASE(test_a_stop_wakes_a_parked_executor) {
    EventRing events = roomy_ring();
    TickGate tick_gate{1, first_tick, events};
    tick_gate.participants[0].deadline_ms = 3'600'000;

    std::thread controller([&] {
        std::this_thread::sleep_for(30ms);
        tick_gate.request_stop();
    });
    const PassResult stopped = tick_gate.pass(first_tick, far());
    controller.join();

    BOOST_TEST(is_type(stopped.blocker, BlockerType::STOP));
}

/**
 * @brief A wait for a tick that has run returns at once, and a wait for one that has not
 *        gives up at its `until`
 */
BOOST_AUTO_TEST_CASE(test_a_wait_for_a_tick_ends_when_it_has_run_or_at_until) {
    EventRing events = roomy_ring();
    TickGate tick_gate{0, first_tick, events};

    BOOST_TEST(tick_gate.wait_for_tick(first_tick, far()));

    const Clock::time_point before = Clock::now();
    BOOST_TEST(!tick_gate.wait_for_tick(first_tick + 1, before + 30ms));
    BOOST_TEST((Clock::now() - before >= 30ms));
    BOOST_TEST(tick_gate.gate.progress_waiters == 0u);

    tick_gate.finish_tick(first_tick);
    BOOST_TEST(tick_gate.wait_for_tick(first_tick + 1, far()));
}

BOOST_AUTO_TEST_SUITE_END()
