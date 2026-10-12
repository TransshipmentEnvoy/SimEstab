/**
 * @file test_engine--session.cpp
 * @brief Tests for the Engine: the session lifecycle, the freeze and step(n)
 *
 * These check the states and the freeze (design_engine_core.md §2.4, design_python_api.md
 * §2), what a step runs and returns (design_python_api.md §4.3), the host endpoint, the
 * engine view handles, both event drains, the snapshot, the control calls and every error
 * the engine raises (design_python_api.md §8). One Engine lives at a time, so each case
 * builds its own. The failed path poisons the process, so its case runs alone, under its
 * own CTest entry.
 *
 * The Engine is exported, but the ring the backlog test fills is not, so this file is a unit
 * of the module (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module sim_estab;
import :limits;
import :engine.event_ring;
import :engine;

using namespace sim_estab::core::engine;
using sim_estab::core::engine::event_ring::EventRing;
using namespace std::chrono_literals;

namespace {

/// A deadline no test reaches, so a lost wake-up shows as a long wait, not a hang
constexpr std::uint32_t ten_seconds_ms = 10'000;

EngineConfig owner_config() {
    EngineConfig config;
    config.event_drain = EventDrain::Owner;
    return config;
}

EngineConfig sink_config() {
    EngineConfig config;
    config.event_drain = EventDrain::Sink;
    return config;
}

EngineViewSpec unpaced_spec(std::string name, std::uint32_t cadence) {
    EngineViewSpec spec;
    spec.name              = std::move(name);
    spec.max_payload_bytes = 16;
    spec.cadence           = cadence;
    return spec;
}

EngineViewSpec paced_spec(std::string name, std::uint32_t cadence, std::uint32_t deadline_ms, OnExpiry on_expiry) {
    EngineViewSpec spec = unpaced_spec(std::move(name), cadence);
    spec.paced          = true;
    spec.deadline_ms    = deadline_ms;
    spec.on_expiry      = on_expiry;
    return spec;
}

/// Everything raised so far, under the owner drain
std::vector<EngineEvent> drained(Engine& engine) {
    std::vector<EngineEvent> out;
    engine.drain_events(out);
    return out;
}

/// A command whose first payload byte is `mark`
Command marked(std::uint8_t mark) {
    Command command;
    command.payload[0] = static_cast<std::byte>(mark);
    return command;
}

CommandOutcome pending(Tick tick) { return {CommandOutcomeType::Pending, tick}; }
CommandOutcome applied(Tick tick) { return {CommandOutcomeType::Applied, tick}; }

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_session_tests)

/**
 * @brief A new engine is Configuring, and every call that needs a session is refused
 */
BOOST_AUTO_TEST_CASE(test_a_new_engine_is_configuring_and_refuses_session_calls) {
    Engine engine{owner_config()};

    BOOST_TEST((engine.state() == EngineState::Configuring));
    BOOST_TEST(engine.config().tick_rate == sim_estab::core::limits::default_tick_rate);
    BOOST_TEST(!engine.stop_requested());

    BOOST_CHECK_THROW((void)engine.step(1), engine_state_error);
    BOOST_CHECK_THROW((void)engine.submit(Command{}), engine_state_error);
    BOOST_CHECK_THROW((void)engine.outcome(CommandHandle{}), engine_state_error);
    BOOST_CHECK_THROW((void)engine.view("ui"), engine_state_error);
    std::vector<EngineEvent> out;
    BOOST_CHECK_THROW(engine.drain_events(out), engine_state_error);
    BOOST_CHECK_THROW((void)engine.sink_digest(), engine_state_error);
    BOOST_CHECK_THROW((void)engine.snapshot(), engine_state_error);
    BOOST_CHECK_THROW((void)engine.seed(), engine_state_error);

    engine.stop_sim_async(); // nothing to stop
    BOOST_TEST((engine.state() == EngineState::Configuring));
}

/**
 * @brief One Engine lives at a time; a destroyed one makes room for the next
 */
BOOST_AUTO_TEST_CASE(test_a_second_engine_in_the_process_is_refused) {
    {
        Engine first{owner_config()};
        BOOST_CHECK_THROW(Engine{owner_config()}, engine_exists_error);
        BOOST_TEST((first.state() == EngineState::Configuring));
    }
    Engine next{owner_config()};
    BOOST_TEST((next.state() == EngineState::Configuring));
}

/**
 * @brief The constructor validates the config, and a refused construction claims nothing
 */
BOOST_AUTO_TEST_CASE(test_the_constructor_validates_the_config) {
    {
        EngineConfig config = owner_config();
        config.tick_rate    = 0;
        BOOST_CHECK_THROW(Engine{config}, engine_error);
    }
    {
        EngineConfig config         = owner_config();
        config.drain_interval_ticks = 0;
        BOOST_CHECK_THROW(Engine{config}, engine_error);
    }
    {
        EngineConfig config           = owner_config();
        config.host_endpoint_capacity = 0;
        BOOST_CHECK_THROW(Engine{config}, engine_error);
    }
    {
        EngineConfig config           = owner_config();
        config.host_endpoint_capacity = sim_estab::core::limits::max_endpoint_capacity + 1;
        BOOST_CHECK_THROW(Engine{config}, engine_error);
    }
    {
        EngineConfig config           = owner_config();
        config.peer_endpoint_capacity = 0;
        BOOST_CHECK_THROW(Engine{config}, engine_error);
    }
    Engine engine{owner_config()}; // the refusals left the process free
    BOOST_TEST((engine.state() == EngineState::Configuring));
}

/**
 * @brief Registration validates the spec and stops at the engine view cap
 */
BOOST_AUTO_TEST_CASE(test_registration_validates_the_spec) {
    Engine engine{owner_config()};

    engine.register_engine_view(unpaced_spec("ui", 1));
    BOOST_CHECK_THROW(engine.register_engine_view(unpaced_spec("ui", 2)), engine_error); // a duplicate name
    BOOST_CHECK_THROW(engine.register_engine_view(unpaced_spec("", 1)), engine_error);
    BOOST_CHECK_THROW(engine.register_engine_view(unpaced_spec("never", 0)), engine_error);

    for (std::uint32_t i = 1; i < sim_estab::core::limits::max_engine_views; ++i) {
        engine.register_engine_view(unpaced_spec("extra" + std::to_string(i), 1));
    }
    BOOST_CHECK_THROW(engine.register_engine_view(unpaced_spec("one too many", 1)), engine_error);
    BOOST_TEST((engine.state() == EngineState::Configuring));
}

/**
 * @brief The freeze refuses a config without an event drain, before anything changes
 */
BOOST_AUTO_TEST_CASE(test_the_freeze_needs_an_event_drain) {
    Engine engine{EngineConfig{}};

    BOOST_CHECK_THROW(engine.start_session(), engine_error);
    BOOST_TEST((engine.state() == EngineState::Configuring));
    engine.register_engine_view(unpaced_spec("ui", 1)); // still Configuring, still open
}

/**
 * @brief The freeze publishes tick 0 into every engine view, raises session.started, keeps
 *        the seed, and closes registration
 */
BOOST_AUTO_TEST_CASE(test_the_freeze_publishes_tick_0_and_starts_the_session) {
    EngineConfig config = owner_config();
    config.seed         = 42;
    Engine engine{config};
    engine.register_engine_view(unpaced_spec("ui", 1));
    engine.register_engine_view(unpaced_spec("slow", 3));

    engine.start_session();

    BOOST_TEST((engine.state() == EngineState::Running));
    BOOST_TEST(engine.seed() == 42u);
    BOOST_TEST((drained(engine) == std::vector<EngineEvent>{{1, EngineEventType::SessionStarted}}));
    BOOST_TEST((engine.snapshot() == Tick{0}));

    EngineViewHandle ui = engine.view("ui");
    BOOST_TEST(ui.name() == "ui");
    BOOST_TEST(ui.cadence() == 1u);
    BOOST_TEST(!ui.is_paced());
    BOOST_TEST(ui.take());
    BOOST_TEST(ui.held().tick == 0u);
    BOOST_TEST(ui.held().size == 0u);
    EngineViewHandle slow = engine.view("slow");
    BOOST_TEST(slow.take());
    BOOST_TEST(slow.held().tick == 0u);

    BOOST_CHECK_THROW(engine.start_session(), engine_state_error);
    BOOST_CHECK_THROW(engine.register_engine_view(unpaced_spec("late", 1)), engine_state_error);
    BOOST_CHECK_THROW((void)engine.view("nothing"), engine_error);
}

/**
 * @brief A session that configures no seed gets one at the freeze and keeps it
 */
BOOST_AUTO_TEST_CASE(test_a_generated_seed_is_kept) {
    Engine engine{owner_config()};
    engine.start_session();

    BOOST_TEST(engine.seed() == engine.seed());
}

/**
 * @brief A step runs its ticks and returns the tick reached; the next step goes on from there
 */
BOOST_AUTO_TEST_CASE(test_a_step_runs_its_ticks_and_returns_the_tick_reached) {
    Engine engine{owner_config()};
    engine.start_session();

    const StepResult result = engine.step(5);
    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST((result.tick_reached == Tick{5}));
    BOOST_TEST(result.interrupted_by.empty());
    BOOST_TEST((engine.snapshot() == Tick{5}));

    BOOST_TEST((engine.step(3).tick_reached == Tick{8}));
    BOOST_TEST((engine.step(0).tick_reached == Tick{8}));
    BOOST_CHECK_THROW((void)engine.step(UINT64_MAX), engine_error); // does not fit
    BOOST_TEST((engine.snapshot() == Tick{8}));
    BOOST_TEST((engine.state() == EngineState::Running));
}

/**
 * @brief A host command runs at the first tick that drains it, and its outcome says which
 */
BOOST_AUTO_TEST_CASE(test_a_host_command_runs_at_the_first_tick_that_drains_it) {
    EngineConfig config           = owner_config();
    config.host_endpoint_capacity = 2;
    Engine engine{config};
    engine.start_session();

    const auto first  = engine.submit(marked(1));
    const auto second = engine.submit(marked(2));
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST((engine.submit(marked(3)) == std::unexpected(Rejection::QueueFull))); // never waits
    BOOST_TEST((engine.outcome(*first) == pending(Tick{0})));

    BOOST_TEST((engine.step(1).tick_reached == Tick{1}));
    BOOST_TEST((engine.outcome(*first) == applied(Tick{1})));
    BOOST_TEST((engine.outcome(*second) == applied(Tick{1})));

    const auto third = engine.submit(marked(3)); // the drain freed the ring
    BOOST_REQUIRE(third.has_value());
    BOOST_TEST((engine.outcome(*third) == pending(Tick{0})));
    BOOST_TEST((engine.step(1).tick_reached == Tick{2}));
    BOOST_TEST((engine.outcome(*third) == applied(Tick{2})));

    // a handle the host endpoint did not issue is refused, not acted on
    BOOST_CHECK_THROW((void)engine.outcome(CommandHandle{1, 0}), engine_error);
    BOOST_CHECK_THROW((void)engine.outcome(CommandHandle{0, 99}), engine_error);
}

/**
 * @brief A stepped tick publishes exactly the engine views due by cadence, and a reader
 *        that falls behind sees the newest publish
 */
BOOST_AUTO_TEST_CASE(test_an_engine_view_is_published_by_cadence) {
    Engine engine{owner_config()};
    engine.register_engine_view(unpaced_spec("every", 1));
    engine.register_engine_view(unpaced_spec("third", 3));
    engine.start_session();
    EngineViewHandle every = engine.view("every");
    EngineViewHandle third = engine.view("third");
    BOOST_TEST(every.take()); // tick 0
    BOOST_TEST(third.take());

    BOOST_TEST((engine.step(7).tick_reached == Tick{7}));
    BOOST_TEST(every.take());
    BOOST_TEST(every.held().tick == 7u); // the publishes between were overwritten: sampled, not consumed
    BOOST_TEST(third.take());
    BOOST_TEST(third.held().tick == 6u);
    BOOST_TEST(!third.take()); // nothing new: the reader keeps what it has
    BOOST_TEST(third.held().tick == 6u);

    BOOST_TEST((engine.step(1).tick_reached == Tick{8})); // not due for `third`
    BOOST_TEST(!third.take());
    BOOST_TEST(every.take());
    BOOST_TEST(every.held().tick == 8u);

    BOOST_TEST((engine.step(1).tick_reached == Tick{9})); // due
    BOOST_TEST(third.take());
    BOOST_TEST(third.held().tick == 9u);
}

/**
 * @brief A paced engine view holds the gate until its reader takes, so a reader on another
 *        thread sees every publish, in order
 */
BOOST_AUTO_TEST_CASE(test_a_paced_engine_view_holds_the_gate_until_its_take) {
    Engine engine{owner_config()};
    engine.register_engine_view(paced_spec("recorder", 2, ten_seconds_ms, OnExpiry::ContinueWithout));
    engine.start_session();
    EngineViewHandle recorder = engine.view("recorder");
    BOOST_TEST(recorder.is_paced());

    std::atomic<bool> done{false};
    std::vector<std::uint64_t> seen;
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            if (recorder.take()) {
                seen.push_back(recorder.held().tick);
            } else {
                std::this_thread::yield();
            }
        }
        while (recorder.take()) {
            seen.push_back(recorder.held().tick);
        }
    });

    const StepResult result = engine.step(10);
    done.store(true, std::memory_order_release);
    reader.join();

    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST((result.tick_reached == Tick{10}));
    BOOST_TEST((seen == std::vector<std::uint64_t>{0, 2, 4, 6, 8, 10}));
}

/**
 * @brief A paced reader that never takes is waited for up to its deadline, then continued
 *        without, and the engine reports it
 */
BOOST_AUTO_TEST_CASE(test_a_late_paced_reader_is_continued_without) {
    Engine engine{owner_config()};
    engine.register_engine_view(paced_spec("recorder", 1, 30, OnExpiry::ContinueWithout));
    engine.start_session();

    const StepResult result = engine.step(3);
    BOOST_TEST((result.end == StepEnd::GrantEnded));
    BOOST_TEST((result.tick_reached == Tick{3}));
    BOOST_TEST((engine.state() == EngineState::Running));
    BOOST_TEST((drained(engine) == std::vector<EngineEvent>{{1, EngineEventType::SessionStarted},
                                                            {1, EngineEventType::ParticipantExpired, 0}}));

    EngineViewHandle recorder = engine.view("recorder"); // its engine view is still published
    BOOST_TEST(recorder.take());
    BOOST_TEST(recorder.held().tick == 3u);
}

/**
 * @brief A stop ends the step after the tick in progress and the session with it
 */
BOOST_AUTO_TEST_CASE(test_a_stop_ends_the_step_and_the_session) {
    Engine engine{owner_config()};
    engine.start_session();
    BOOST_TEST((engine.step(2).tick_reached == Tick{2}));

    engine.request_stop();
    BOOST_TEST(engine.stop_requested());
    const StepResult result = engine.step(10);
    BOOST_TEST((result.end == StepEnd::Stopped));
    BOOST_TEST((result.tick_reached == Tick{2}));
    BOOST_TEST((engine.state() == EngineState::Stopped));

    BOOST_CHECK_THROW((void)engine.step(1), engine_state_error);
    BOOST_TEST((engine.snapshot() == Tick{2})); // reads still work
    BOOST_TEST(engine.submit(marked(1)).has_value());
    BOOST_TEST((drained(engine) ==
                std::vector<EngineEvent>{{1, EngineEventType::SessionStarted}, {3, EngineEventType::SessionStopped}}));
    engine.stop_sim_async(); // already stopped: nothing to do
    BOOST_TEST((engine.state() == EngineState::Stopped));
}

/**
 * @brief stop_sim_async() ends ticking for the session
 */
BOOST_AUTO_TEST_CASE(test_stop_sim_async_ends_ticking) {
    Engine engine{owner_config()};
    engine.start_session();
    BOOST_TEST((engine.step(1).tick_reached == Tick{1}));

    engine.stop_sim_async();
    BOOST_TEST((engine.state() == EngineState::Stopped));
    BOOST_TEST(engine.stop_requested());
    BOOST_CHECK_THROW((void)engine.step(1), engine_state_error);
    engine.pause(); // a control call on a stopped session is harmless
    BOOST_TEST((engine.snapshot() == Tick{1}));
}

/**
 * @brief Under an owner drain, a step that reaches the event backlog mark raises, naming the
 *        tick reached, and goes on once the backlog is drained
 */
BOOST_AUTO_TEST_CASE(test_the_event_backlog_mark_raises_from_a_step) {
    EngineConfig config         = owner_config();
    config.drain_interval_ticks = 1; // the mark is one tick's worth of events: E
    Engine engine{config};
    engine.start_session();
    EventRing& events = session_events(engine);
    BOOST_TEST(events.high_water == 5u); // E = T·S + S + 3·P + V + 4 with one source and nothing else

    // An M1 session raises nothing per tick by itself; raise one tick's worth by hand.
    for (std::uint64_t i = 0; i < events.high_water; ++i) {
        events.enqueue({1, EngineEventType::ParticipantExpired, 9});
    }
    try {
        (void)engine.step(3);
        BOOST_FAIL("a step at the backlog mark must raise event_backlog_error");
    } catch (const event_backlog_error& error) {
        BOOST_TEST((error.tick_reached() == Tick{0}));
    }
    BOOST_TEST((engine.state() == EngineState::Running));

    const std::vector<EngineEvent> batch = drained(engine); // session.started, the five, backlog_paused
    BOOST_TEST(batch.size() == 7u);
    BOOST_TEST((batch.front() == EngineEvent{1, EngineEventType::SessionStarted}));
    BOOST_TEST((batch.back() == EngineEvent{1, EngineEventType::SimBacklogPaused}));

    BOOST_TEST((engine.step(3).tick_reached == Tick{3}));
    BOOST_TEST((drained(engine) == std::vector<EngineEvent>{{1, EngineEventType::SimBacklogResumed}}));
}

/**
 * @brief Step callbacks run in key order, once per slice, with the tick reached; the first
 *        that returns true ends the step and names itself
 */
BOOST_AUTO_TEST_CASE(test_step_callbacks_run_in_key_order_and_end_the_step) {
    std::vector<std::string> order;
    std::uint64_t a_calls = 0;
    Tick a_tick;
    StepCallbacks callbacks;
    callbacks.emplace("b", [&](Tick) {
        order.push_back("b");
        return false;
    });
    callbacks.emplace("a", [&](Tick tick) {
        order.push_back("a");
        a_tick = tick;
        return ++a_calls == 2;
    });
    Engine engine{owner_config(), std::move(callbacks)};
    engine.start_session();

    const StepResult result = engine.step(1'000'000'000'000);
    BOOST_TEST((result.end == StepEnd::Interrupted));
    BOOST_TEST(result.interrupted_by == "a");
    BOOST_TEST((order == std::vector<std::string>{"a", "b", "a"})); // "b" is not called after "a" ended the step
    BOOST_TEST((a_tick == result.tick_reached));
    BOOST_TEST((engine.snapshot() == result.tick_reached));
    BOOST_TEST((engine.state() == EngineState::Running));
}

/**
 * @brief A step callback that throws ends the step cleanly and propagates; the engine goes
 *        on
 */
BOOST_AUTO_TEST_CASE(test_a_throwing_step_callback_propagates_and_the_engine_goes_on) {
    std::uint64_t calls = 0;
    StepCallbacks callbacks;
    callbacks.emplace("boom", [&](Tick) -> bool {
        if (++calls == 1) {
            throw std::runtime_error("boom");
        }
        return true;
    });
    Engine engine{owner_config(), std::move(callbacks)};
    engine.start_session();

    BOOST_CHECK_THROW((void)engine.step(1'000'000'000'000), std::runtime_error);
    BOOST_TEST((engine.state() == EngineState::Running));
    const Tick reached = engine.snapshot();
    BOOST_TEST((reached > Tick{0}));

    const StepResult result = engine.step(1'000'000'000'000);
    BOOST_TEST((result.end == StepEnd::Interrupted));
    BOOST_TEST(result.interrupted_by == "boom");
    BOOST_TEST((result.tick_reached > reached));
}

/**
 * @brief A control call made while Configuring applies at the freeze
 */
BOOST_AUTO_TEST_CASE(test_control_calls_before_the_freeze_apply_at_it) {
    {
        Engine engine{owner_config()};
        engine.request_stop();
        BOOST_TEST(engine.stop_requested());
        engine.start_session();
        BOOST_TEST(engine.stop_requested());

        const StepResult result = engine.step(5);
        BOOST_TEST((result.end == StepEnd::Stopped));
        BOOST_TEST((result.tick_reached == Tick{0}));
        BOOST_TEST((engine.state() == EngineState::Stopped));
    }
    {
        Engine engine{owner_config()};
        engine.pause();
        engine.resume(); // cancels the pending pause
        engine.pause();
        engine.start_session();
        BOOST_TEST((engine.step(2).tick_reached == Tick{2})); // a step's grant overrides a pause
    }
}

/**
 * @brief While a step is in flight, a resume, a snapshot and a second step are refused
 */
BOOST_AUTO_TEST_CASE(test_resume_snapshot_and_step_are_refused_during_a_step) {
    Engine *self = nullptr;
    StepCallbacks callbacks;
    callbacks.emplace("probe", [&](Tick tick) {
        BOOST_CHECK_THROW(self->resume(), engine_state_error);
        BOOST_CHECK_THROW((void)self->snapshot(), engine_state_error);
        BOOST_CHECK_THROW((void)self->step(1), engine_state_error);
        self->pause(); // allowed: it ends the grant, like the callback itself
        BOOST_TEST((tick > Tick{0}));
        return true;
    });
    Engine engine{owner_config(), std::move(callbacks)};
    self = &engine;
    engine.start_session();

    const StepResult result = engine.step(1'000'000'000'000);
    BOOST_TEST((result.end == StepEnd::Interrupted));
    BOOST_TEST((engine.snapshot() == result.tick_reached));
    engine.resume(); // the step is over
    BOOST_TEST((engine.step(1).tick_reached == result.tick_reached + 1));
}

/**
 * @brief Under a sink drain the engine empties the ring itself and counts; drain_events()
 *        is refused, and sink_digest() is refused under an owner drain
 */
BOOST_AUTO_TEST_CASE(test_the_sink_drain_counts_and_refuses_drain_events) {
    {
        Engine engine{sink_config()};
        engine.start_session();
        std::vector<EngineEvent> out;
        BOOST_CHECK_THROW(engine.drain_events(out), engine_state_error);
        BOOST_TEST(engine.sink_digest().count == 0u); // session.started waits in the side list

        BOOST_TEST((engine.step(1).tick_reached == Tick{1}));
        BOOST_TEST(engine.sink_digest().count == 1u);
        engine.request_stop();
        BOOST_TEST((engine.step(1).end == StepEnd::Stopped));
        BOOST_TEST(engine.sink_digest().count == 2u); // session.stopped, counted when the step ended
    }
    Engine engine{owner_config()};
    engine.start_session();
    BOOST_CHECK_THROW((void)engine.sink_digest(), engine_state_error);
}

/**
 * @brief A participant's deadline under Fail ends the step in the terminal Failed state,
 *        every later call fails, and the process stays poisoned after the engine is destroyed
 *
 * It poisons the process, so it runs alone: its own CTest entry runs it, and the full run
 * leaves it out by its label.
 */
BOOST_AUTO_TEST_CASE(test_the_failed_path_poisons_the_process, *boost::unit_test::label("poison")) {
    {
        Engine engine{owner_config()};
        engine.register_engine_view(paced_spec("recorder", 1, 30, OnExpiry::Fail));
        engine.start_session();

        BOOST_CHECK_THROW((void)engine.step(5), engine_failed_error);
        BOOST_TEST((engine.state() == EngineState::Failed));
        BOOST_CHECK_THROW((void)engine.step(1), engine_failed_error);
        BOOST_CHECK_THROW((void)engine.submit(marked(1)), engine_failed_error);
        BOOST_CHECK_THROW((void)engine.snapshot(), engine_failed_error);
        BOOST_CHECK_THROW(engine.pause(), engine_failed_error);
        BOOST_CHECK_THROW(engine.request_stop(), engine_failed_error);
        BOOST_CHECK_THROW(engine.stop_sim_async(), engine_failed_error);
        engine.close(); // a no-op in Failed, and it does not raise
        BOOST_TEST((engine.state() == EngineState::Failed));
    }
    BOOST_CHECK_THROW(Engine{owner_config()}, engine_exists_error); // the process is poisoned
}

BOOST_AUTO_TEST_SUITE_END()
