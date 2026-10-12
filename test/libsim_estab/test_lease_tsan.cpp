/**
 * @file test_lease_tsan.cpp
 * @brief Race tests for the operation lease: close() against every any-thread call
 *
 * close() closes the lease, waits for the running calls and releases the session. The calls
 * that can overlap it run on other threads: the control calls, and the engine view handle's
 * take and held (design_python_api.md §2, §6). Every such call must either finish before the
 * release or be refused with engine_closed_error, and never touch the session after it is
 * gone. On x86-64 a wrongly ordered lease still passes this test; it proves something only
 * under ThreadSanitizer, where an access overlapping the release is a reported race
 * (design_engine_core.md §5.1).
 *
 * The Engine is exported, but this file joins the module like the other race tests
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

module sim_estab;
import :engine;

using namespace sim_estab::core::engine;

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

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(lease_tsan_tests)

/**
 * @brief close() racing the control calls and a handle on other threads: each call finishes
 *        or is refused as closed, and the session is released only once they have left
 */
BOOST_AUTO_TEST_CASE(test_close_races_the_any_thread_calls) {
    constexpr std::uint32_t rounds = 20;

    for (std::uint32_t round = 0; round < rounds; ++round) {
        Engine engine{owner_config()};
        engine.register_engine_view(unpaced_spec("ui"));
        engine.start_session();
        EngineViewHandle ui = engine.view("ui");

        std::atomic<bool> stop{false};
        std::atomic<std::uint32_t> unexpected{0};
        std::atomic<std::uint64_t> control_calls{0};
        std::atomic<std::uint64_t> takes{0};
        std::thread control([&] {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    engine.pause();
                    (void)engine.stop_requested();
                    engine.resume();
                    control_calls.fetch_add(1, std::memory_order_relaxed);
                } catch (const engine_state_error&) {
                    // resume() while a step is in flight: a refusal, not a race
                } catch (const engine_closed_error&) {
                    break;
                } catch (...) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        });
        std::thread reader([&] {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    if (ui.take()) {
                        takes.fetch_add(ui.held().tick != 0 ? 1 : 0, std::memory_order_relaxed);
                    }
                } catch (const engine_closed_error&) {
                    break;
                } catch (...) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        });

        for (std::uint32_t i = 0; i < 50; ++i) {
            (void)engine.step(1);
        }
        engine.close();
        stop.store(true, std::memory_order_release);
        control.join();
        reader.join();

        BOOST_TEST((engine.state() == EngineState::Closed));
        BOOST_TEST(unexpected.load() == 0u);
        BOOST_CHECK_THROW((void)ui.take(), engine_closed_error);
        BOOST_CHECK_THROW(engine.pause(), engine_closed_error);
    }
}

BOOST_AUTO_TEST_SUITE_END()
