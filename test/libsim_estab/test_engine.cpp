/**
 * @file test_engine.cpp
 * @brief Tests for the tick loop
 *
 * These check what a step runs and what ends it: its grant, a stop, a pause, the event
 * backlog mark, an interrupt and a participant's deadline (design_engine_core.md §3.3,
 * design_python_api.md §4.3). The tick is a stand-in that records which ticks ran.
 *
 * The tick loop is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

module sim_estab;
import :engine.gate;
import :engine;

using namespace sim_estab::core::engine;
using sim_estab::core::engine::gate::OnExpiry;
using sim_estab::core::engine::gate::TickGate;
using namespace std::chrono_literals;

namespace {

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

bool never_interrupted() { return false; }

/// True if a step has left the gate as it must: no step in flight, and no grant left
bool is_at_rest(const TickGate& tick_gate) {
    return !tick_gate.host.step_in_flight && tick_gate.host.run_until.load() == tick_gate.first_unexecuted.load();
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_tests)

/**
 * @brief A step runs its ticks in order and reports the last one
 */
BOOST_AUTO_TEST_CASE(test_a_step_runs_its_ticks_in_order) {
    TickGate tick_gate{0, first_tick};
    std::vector<std::uint64_t> ran;

    const StepResult result = run_step(tick_gate, 5, [&](Tick tick) { ran.push_back(tick.value); }, never_interrupted);

    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST(result.tick_reached.value == 5u);
    BOOST_TEST((ran == std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
    BOOST_TEST(tick_gate.first_unexecuted.load() == 6u);
    BOOST_TEST(is_at_rest(tick_gate));
}

/**
 * @brief A step starts where the last one ended
 */
BOOST_AUTO_TEST_CASE(test_a_step_continues_where_the_last_one_ended) {
    TickGate tick_gate{0, first_tick};
    std::vector<std::uint64_t> ran;
    const auto record = [&](Tick tick) { ran.push_back(tick.value); };

    BOOST_TEST(run_step(tick_gate, 2, record, never_interrupted).tick_reached.value == 2u);
    BOOST_TEST(run_step(tick_gate, 3, record, never_interrupted).tick_reached.value == 5u);
    BOOST_TEST((ran == std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
}

/**
 * @brief A step of no ticks runs nothing
 */
BOOST_AUTO_TEST_CASE(test_a_step_of_no_ticks_runs_nothing) {
    TickGate tick_gate{0, first_tick};
    std::uint64_t ticks = 0;

    const StepResult result = run_step(tick_gate, 0, [&](Tick) { ++ticks; }, never_interrupted);

    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST(result.tick_reached.value == first_tick.value - 1);
    BOOST_TEST(ticks == 0u);
    BOOST_TEST(is_at_rest(tick_gate));
}

/**
 * @brief A stop ends the step after the tick in progress, and no later step runs a tick
 */
BOOST_AUTO_TEST_CASE(test_a_stop_ends_the_step) {
    TickGate tick_gate{0, first_tick};
    std::uint64_t ticks  = 0;
    const auto stop_at_3 = [&](Tick tick) {
        ++ticks;
        if (tick == Tick{3}) {
            tick_gate.request_stop();
        }
    };

    const StepResult result = run_step(tick_gate, 10, stop_at_3, never_interrupted);
    BOOST_TEST((result.end == StepEnd::Stopped));
    BOOST_TEST(result.tick_reached.value == 3u);
    BOOST_TEST(ticks == 3u);
    BOOST_TEST(is_at_rest(tick_gate));

    const StepResult again = run_step(tick_gate, 10, stop_at_3, never_interrupted);
    BOOST_TEST((again.end == StepEnd::Stopped));
    BOOST_TEST(again.tick_reached.value == 3u);
    BOOST_TEST(ticks == 3u);
}

/**
 * @brief A pause ends the step after the tick in progress, and a later step goes on
 */
BOOST_AUTO_TEST_CASE(test_a_pause_ends_the_step_early) {
    TickGate tick_gate{0, first_tick};
    std::uint64_t ticks = 0;

    const StepResult result = run_step(
        tick_gate, 10,
        [&](Tick tick) {
            ++ticks;
            if (tick == Tick{2}) {
                tick_gate.pause();
            }
        },
        never_interrupted);
    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST(result.tick_reached.value == 2u);
    BOOST_TEST(ticks == 2u);
    BOOST_TEST(is_at_rest(tick_gate));

    BOOST_TEST(run_step(tick_gate, 2, [&](Tick) { ++ticks; }, never_interrupted).tick_reached.value == 4u);
    BOOST_TEST(ticks == 4u);
}

/**
 * @brief The event backlog mark ends the step, and the step goes on once the backlog is
 *        drained
 */
BOOST_AUTO_TEST_CASE(test_the_backlog_mark_ends_the_step) {
    TickGate tick_gate{0, first_tick};
    tick_gate.high_water = 3;
    const auto one_event = [&](Tick) { ++tick_gate.backlog; };

    const StepResult result = run_step(tick_gate, 10, one_event, never_interrupted);
    BOOST_TEST((result.end == StepEnd::EventBacklog));
    BOOST_TEST(result.tick_reached.value == 3u);
    BOOST_TEST(is_at_rest(tick_gate));

    tick_gate.backlog      = 0; // the caller drains
    const StepResult again = run_step(tick_gate, 2, one_event, never_interrupted);
    BOOST_TEST((again.end == StepEnd::GrantEnded));
    BOOST_TEST(again.tick_reached.value == 5u);
}

/**
 * @brief An interrupt ends the step at a tick boundary, and the check runs once per slice
 */
BOOST_AUTO_TEST_CASE(test_an_interrupt_ends_the_step_at_a_tick_boundary) {
    TickGate tick_gate{0, first_tick};
    std::uint64_t ticks  = 0;
    std::uint64_t checks = 0;

    const TickGate::Clock::time_point before = TickGate::Clock::now();
    const StepResult result                  = run_step(
        tick_gate, 1'000'000'000'000, [&](Tick) { ++ticks; },
        [&] {
            ++checks;
            return true;
        });

    BOOST_TEST((result.end == StepEnd::Interrupted));
    BOOST_TEST(checks == 1u);
    BOOST_TEST((TickGate::Clock::now() - before >= interrupt_check_slice));
    BOOST_TEST(ticks > 0u);
    BOOST_TEST(result.tick_reached.value == ticks);
    BOOST_TEST(tick_gate.first_unexecuted.load() == ticks + 1);
    BOOST_TEST(is_at_rest(tick_gate));
}

/**
 * @brief An interrupt ends a step that waits at the gate, between two slices
 */
BOOST_AUTO_TEST_CASE(test_an_interrupt_ends_a_step_that_waits_at_the_gate) {
    TickGate tick_gate{1, first_tick};
    tick_gate.participants[0].deadline_ms = 3'600'000;
    std::uint64_t ticks                   = 0;
    std::uint64_t checks                  = 0;

    const StepResult result = run_step(
        tick_gate, 10, [&](Tick) { ++ticks; },
        [&] {
            ++checks;
            return checks == 2;
        });

    BOOST_TEST((result.end == StepEnd::Interrupted));
    BOOST_TEST(checks == 2u);
    BOOST_TEST(ticks == 0u);
    BOOST_TEST(result.tick_reached.value == first_tick.value - 1);
    BOOST_TEST(is_at_rest(tick_gate));
}

/**
 * @brief A step after an interrupted wait gives the participant its whole deadline again
 *
 * Between steps the grant is revoked, which is a pause, and a pause discards the deadline
 * clock (design_engine_core.md §3.3).
 */
BOOST_AUTO_TEST_CASE(test_a_step_after_an_interrupted_wait_gets_a_whole_deadline) {
    TickGate tick_gate{1, first_tick};
    tick_gate.participants[0].deadline_ms = 300;
    tick_gate.participants[0].on_expiry   = OnExpiry::Fail;
    const auto interrupt_at_first_check   = [] { return true; };

    const StepResult first = run_step(tick_gate, 1, [](Tick) {}, interrupt_at_first_check);
    BOOST_TEST((first.end == StepEnd::Interrupted));
    BOOST_TEST(!tick_gate.wait_started.has_value());

    std::this_thread::sleep_for(400ms); // longer than the deadline, all of it between steps

    // a deadline still counted from the first step would have passed, and the step would fail
    const StepResult second = run_step(tick_gate, 1, [](Tick) {}, interrupt_at_first_check);
    BOOST_TEST((second.end == StepEnd::Interrupted));
}

/**
 * @brief An interrupt check that throws while the step waits at the gate leaves no deadline
 *        clock behind
 */
BOOST_AUTO_TEST_CASE(test_a_throwing_interrupt_check_leaves_no_deadline_clock) {
    TickGate tick_gate{1, first_tick};
    tick_gate.participants[0].deadline_ms = 3'600'000;
    const auto throw_at_first_check       = []() -> bool { throw std::runtime_error("interrupted"); };

    BOOST_CHECK_THROW((void)run_step(tick_gate, 1, [](Tick) {}, throw_at_first_check), std::runtime_error);

    BOOST_TEST(!tick_gate.wait_started.has_value());
    BOOST_TEST(is_at_rest(tick_gate));
}

/**
 * @brief A participant's deadline that passes under Fail ends the step and names the
 *        participant
 */
BOOST_AUTO_TEST_CASE(test_a_deadline_under_fail_ends_the_step) {
    TickGate tick_gate{2, first_tick};
    tick_gate.participants[0].deadline_ms = 3'600'000;
    tick_gate.participants[1].deadline_ms = 30;
    tick_gate.participants[1].on_expiry   = OnExpiry::Fail;
    tick_gate.declare_ready(0, Tick{UINT64_MAX});
    tick_gate.declare_ready(1, Tick{2});
    std::uint64_t ticks = 0;

    const StepResult result = run_step(tick_gate, 10, [&](Tick) { ++ticks; }, never_interrupted);

    BOOST_TEST((result.end == StepEnd::Failed));
    BOOST_TEST(result.participant == 1u);
    BOOST_TEST(result.tick_reached.value == 2u);
    BOOST_TEST(ticks == 2u);
    BOOST_TEST(is_at_rest(tick_gate));
}

/**
 * @brief A step goes on without a participant whose deadline passes under ContinueWithout
 */
BOOST_AUTO_TEST_CASE(test_a_step_continues_without_a_late_participant) {
    TickGate tick_gate{1, first_tick};
    tick_gate.participants[0].deadline_ms = 30;
    tick_gate.participants[0].on_expiry   = OnExpiry::ContinueWithout;
    std::uint64_t ticks                   = 0;

    const StepResult result = run_step(tick_gate, 5, [&](Tick) { ++ticks; }, never_interrupted);

    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST(result.tick_reached.value == 5u);
    BOOST_TEST(ticks == 5u);
    BOOST_TEST(tick_gate.participants[0].active.load() == 0u);
}

/**
 * @brief A step inside a step is refused, and the outer step goes on
 */
BOOST_AUTO_TEST_CASE(test_a_step_inside_a_step_is_refused) {
    TickGate tick_gate{0, first_tick};
    std::uint64_t inner_ticks = 0;
    StepResult inner;

    const StepResult outer = run_step(
        tick_gate, 3,
        [&](Tick tick) {
            if (tick == Tick{2}) {
                inner = run_step(tick_gate, 1, [&](Tick) { ++inner_ticks; }, never_interrupted);
            }
        },
        never_interrupted);

    BOOST_TEST((inner.end == StepEnd::StepInFlight));
    BOOST_TEST(inner.tick_reached.value == 1u);
    BOOST_TEST(inner_ticks == 0u);
    BOOST_TEST((outer.end == StepEnd::GrantEnded));
    BOOST_TEST(outer.tick_reached.value == 3u);
    BOOST_TEST(is_at_rest(tick_gate));
}

/**
 * @brief A step whose tick count does not fit is refused and runs nothing
 */
BOOST_AUTO_TEST_CASE(test_a_step_that_does_not_fit_is_refused) {
    TickGate tick_gate{0, first_tick};
    std::uint64_t ticks = 0;

    const StepResult result = run_step(tick_gate, UINT64_MAX, [&](Tick) { ++ticks; }, never_interrupted);

    BOOST_TEST((result.end == StepEnd::Overflow));
    BOOST_TEST(result.tick_reached.value == first_tick.value - 1);
    BOOST_TEST(ticks == 0u);
    BOOST_TEST(!tick_gate.host.step_in_flight);
}

/**
 * @brief A tick that throws still ends the step: the gate is left at rest
 */
BOOST_AUTO_TEST_CASE(test_a_tick_that_throws_ends_the_step) {
    TickGate tick_gate{0, first_tick};
    const auto throw_at_2 = [](Tick tick) {
        if (tick == Tick{2}) {
            throw std::runtime_error("tick 2");
        }
    };

    BOOST_CHECK_THROW((void)run_step(tick_gate, 5, throw_at_2, never_interrupted), std::runtime_error);

    BOOST_TEST(tick_gate.first_unexecuted.load() == 2u);
    BOOST_TEST(is_at_rest(tick_gate));
}

BOOST_AUTO_TEST_SUITE_END()
