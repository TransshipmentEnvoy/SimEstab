/**
 * @file test_engine--close.cpp
 * @brief Tests for the Engine's close(): the operation lease and the shutdown
 *
 * These check what close() does from each state, that it never raises and can be repeated,
 * what a call and a handle see once the engine is closing, closed or gone, and the failed
 * path of a close that times out (design_python_api.md §2, §6). One Engine lives at a time,
 * so each case builds its own. The timed-out close poisons the process, so its case runs
 * alone, under its own CTest entry.
 *
 * The timeout case holds the operation lease through a seam the module keeps for its
 * tests, so this file is a unit of the module (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module sim_estab;
import :engine.lease;
import :engine;

using namespace sim_estab::core::engine;
using sim_estab::core::engine::lease::Lease;
using sim_estab::core::engine::lease::LeaseState;

namespace {

EngineConfig owner_config() {
    EngineConfig config;
    config.event_drain = EventDrain::Owner;
    return config;
}

EngineViewSpec unpaced_spec(std::string name) {
    EngineViewSpec spec;
    spec.name              = std::move(name);
    spec.max_payload_bytes = 16;
    spec.cadence           = 1;
    return spec;
}

/// Every call that needs the operation lease throws engine_closed_error
void check_every_call_is_refused_as_closed(Engine& engine, EngineViewHandle *handle) {
    BOOST_CHECK_THROW((void)engine.step(1), engine_closed_error);
    BOOST_CHECK_THROW((void)engine.submit(Command{}), engine_closed_error);
    BOOST_CHECK_THROW((void)engine.outcome(CommandHandle{}), engine_closed_error);
    BOOST_CHECK_THROW((void)engine.view("ui"), engine_closed_error);
    std::vector<EngineEvent> out;
    BOOST_CHECK_THROW(engine.drain_events(out), engine_closed_error);
    BOOST_CHECK_THROW((void)engine.sink_digest(), engine_closed_error);
    BOOST_CHECK_THROW((void)engine.snapshot(), engine_closed_error);
    BOOST_CHECK_THROW((void)engine.seed(), engine_closed_error);
    BOOST_CHECK_THROW(engine.register_engine_view(unpaced_spec("late")), engine_closed_error);
    BOOST_CHECK_THROW(engine.start_session(), engine_closed_error);
    BOOST_CHECK_THROW(engine.pause(), engine_closed_error);
    BOOST_CHECK_THROW(engine.resume(), engine_closed_error);
    BOOST_CHECK_THROW(engine.request_stop(), engine_closed_error);
    BOOST_CHECK_THROW(engine.stop_sim_async(), engine_closed_error);
    BOOST_CHECK_THROW(engine.set_log_level(sim_estab::core::log::severity_level::trace), engine_closed_error);
    // the query never throws: close() requested the stop, and the request is sticky
    // (design_python_api.md §2)
    BOOST_TEST(engine.stop_requested());
    if (handle != nullptr) {
        BOOST_CHECK_THROW((void)handle->take(), engine_closed_error);
        BOOST_CHECK_THROW((void)handle->held(), engine_closed_error);
        BOOST_TEST(handle->name() == "ui"); // plain data, no engine behind it
    }
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_close_tests)

/**
 * @brief close() from Configuring closes with nothing to release, refuses every call, and a
 *        second close does nothing
 */
BOOST_AUTO_TEST_CASE(test_close_from_configuring) {
    Engine engine{owner_config()};
    engine.register_engine_view(unpaced_spec("ui"));

    engine.close();
    BOOST_TEST((engine.state() == EngineState::Closed));
    check_every_call_is_refused_as_closed(engine, nullptr);
    engine.close();
    BOOST_TEST((engine.state() == EngineState::Closed));
}

/**
 * @brief close() from Running stops the session, releases it, and refuses every call and the
 *        handle afterwards
 */
BOOST_AUTO_TEST_CASE(test_close_from_running) {
    Engine engine{owner_config()};
    engine.register_engine_view(unpaced_spec("ui"));
    engine.start_session();
    EngineViewHandle ui = engine.view("ui");
    BOOST_TEST((engine.step(3).tick_reached == Tick{3}));
    BOOST_TEST(engine.submit(Command{}).has_value());
    BOOST_TEST(ui.take());

    engine.close();
    BOOST_TEST((engine.state() == EngineState::Closed));
    check_every_call_is_refused_as_closed(engine, &ui);
    engine.close();
    BOOST_TEST((engine.state() == EngineState::Closed));
}

/**
 * @brief close() after stop_sim_async() releases a stopped session
 */
BOOST_AUTO_TEST_CASE(test_close_from_stopped) {
    Engine engine{owner_config()};
    engine.start_session();
    BOOST_TEST((engine.step(2).tick_reached == Tick{2}));
    engine.stop_sim_async();
    BOOST_TEST((engine.state() == EngineState::Stopped));

    engine.close();
    BOOST_TEST((engine.state() == EngineState::Closed));
    check_every_call_is_refused_as_closed(engine, nullptr);
}

/**
 * @brief A closed engine is not live: the next Engine may be constructed while the closed
 *        one still exists (design_python_api.md §2)
 */
BOOST_AUTO_TEST_CASE(test_a_closed_engine_releases_the_singleton) {
    Engine first{owner_config()};
    first.start_session();
    BOOST_CHECK_THROW(Engine{owner_config()}, engine_exists_error);
    first.close();
    BOOST_TEST((first.state() == EngineState::Closed));

    Engine second{owner_config()}; // `first` still exists
    second.start_session();
    BOOST_TEST((second.step(1).tick_reached == Tick{1}));
    check_every_call_is_refused_as_closed(first, nullptr); // and stays closed
    second.close();
    BOOST_TEST((second.state() == EngineState::Closed));
}

/**
 * @brief A handle that outlives its engine sees the engine closed instead of freed memory;
 *        the destructor closes when the caller did not
 */
BOOST_AUTO_TEST_CASE(test_a_handle_outlives_its_engine_safely) {
    std::optional<EngineViewHandle> ui;
    {
        Engine engine{owner_config()};
        engine.register_engine_view(unpaced_spec("ui"));
        engine.start_session();
        ui.emplace(engine.view("ui"));
        BOOST_TEST((engine.step(1).tick_reached == Tick{1}));
        BOOST_TEST(ui->take());
        BOOST_TEST(ui->held().tick == 1u);
        // destroyed without close(): the destructor closes
    }
    BOOST_CHECK_THROW((void)ui->take(), engine_closed_error);
    BOOST_CHECK_THROW((void)ui->held(), engine_closed_error);

    Engine next{owner_config()}; // the process is free: the close succeeded
    BOOST_TEST((next.state() == EngineState::Configuring));
}

/**
 * @brief A close that times out waiting for a running call fails the engine, keeps the
 *        session, and poisons the process
 *
 * It poisons the process, so it runs alone: its own CTest entry runs it, and the full run
 * leaves it out by its label.
 */
BOOST_AUTO_TEST_CASE(test_a_close_that_times_out_fails_and_poisons, *boost::unit_test::label("poison")) {
    {
        EngineConfig config      = owner_config();
        config.shutdown_deadline = std::chrono::duration<double>{0.05};
        Engine engine{config};
        engine.register_engine_view(unpaced_spec("ui"));
        engine.start_session();
        EngineViewHandle ui = engine.view("ui");

        Lease& control = engine_control(engine);
        BOOST_REQUIRE(control.try_enter()); // a call that never returns

        engine.close(); // never raises
        BOOST_TEST((engine.state() == EngineState::Failed));
        BOOST_TEST((control.state.load() == LeaseState::FAILED));
        BOOST_CHECK_THROW((void)engine.step(1), engine_failed_error);
        BOOST_CHECK_THROW((void)ui.take(), engine_failed_error);
        BOOST_CHECK_THROW(engine.pause(), engine_failed_error);
        BOOST_TEST(engine.stop_requested()); // the query never throws; no tick runs again
        engine.close(); // a no-op in Failed
        BOOST_TEST((engine.state() == EngineState::Failed));
        control.leave();
    }
    BOOST_CHECK_THROW(Engine{owner_config()}, engine_exists_error); // the process is poisoned
}

BOOST_AUTO_TEST_SUITE_END()
