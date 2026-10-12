/**
 * @file test_engine_lease.cpp
 * @brief Tests for the lease, the building block of the endpoint lease and the operation
 *        lease
 *
 * These check what each call does: which states admit a holder, how the count moves, what
 * the guard does on every way out, and when the closer's wait returns
 * (design_engine_core.md §5.1, design_python_api.md §6). They cannot tell a correctly
 * ordered implementation from a wrongly ordered one: the litmus tests in test/litmus/ do
 * that with herd7, and test_close_wait_stress.cpp drives the closer's park.
 *
 * sim_estab:engine.lease is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

module sim_estab;
import :engine.lease;

using namespace sim_estab::core::engine::lease;
using namespace std::chrono_literals;

namespace {

using Clock = Lease::Clock;

/// An `until` already passed: the closer checks once and returns
Clock::time_point now() { return Clock::now(); }

/// An `until` that no test reaches
Clock::time_point far() { return Clock::now() + 1h; }

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_lease_tests)

/**
 * @brief An open lease admits holders and counts them in and out
 */
BOOST_AUTO_TEST_CASE(test_an_open_lease_admits_and_counts) {
    Lease lease{LeaseState::OPEN};

    BOOST_TEST(lease.try_enter());
    BOOST_TEST(lease.active.load() == 1u);
    BOOST_TEST(lease.try_enter());
    BOOST_TEST(lease.active.load() == 2u);
    lease.leave();
    lease.leave();
    BOOST_TEST(lease.active.load() == 0u);
    BOOST_TEST((lease.state.load() == LeaseState::OPEN));
}

/**
 * @brief A lease that is not open refuses every holder, and counts none
 */
BOOST_AUTO_TEST_CASE(test_a_lease_that_is_not_open_refuses) {
    for (const LeaseState state : {LeaseState::CLOSING, LeaseState::CLOSED, LeaseState::FAILED}) {
        Lease lease{state};
        BOOST_TEST(!lease.try_enter());
        BOOST_TEST(lease.active.load() == 0u);
        BOOST_TEST((lease.state.load() == state));
    }
}

/**
 * @brief The guard leaves on every way out: a return and a throw
 */
BOOST_AUTO_TEST_CASE(test_the_guard_leaves_on_every_way_out) {
    Lease lease{LeaseState::OPEN};
    {
        BOOST_REQUIRE(lease.try_enter());
        const LeaseGuard guard{lease};
        BOOST_TEST(lease.active.load() == 1u);
    }
    BOOST_TEST(lease.active.load() == 0u);

    BOOST_CHECK_THROW(
        [&] {
            BOOST_REQUIRE(lease.try_enter());
            const LeaseGuard guard{lease};
            throw std::runtime_error("out through an exception");
        }(),
        std::runtime_error);
    BOOST_TEST(lease.active.load() == 0u);
}

/**
 * @brief Once the lease is closing, a holder that arrives is refused and leaves no count
 */
BOOST_AUTO_TEST_CASE(test_a_holder_after_closing_is_not_counted) {
    Lease lease{LeaseState::OPEN};
    BOOST_REQUIRE(lease.try_enter());
    lease.state.exchange(LeaseState::CLOSING, std::memory_order_acq_rel);

    BOOST_TEST(!lease.try_enter());
    BOOST_TEST(lease.active.load() == 1u); // the holder from before the close
    lease.leave();
    BOOST_TEST(lease.active.load() == 0u);
}

/**
 * @brief The closer returns at once when no holder is running
 */
BOOST_AUTO_TEST_CASE(test_the_closer_returns_at_once_without_holders) {
    Lease lease{LeaseState::OPEN};
    lease.state.exchange(LeaseState::CLOSING, std::memory_order_acq_rel);

    BOOST_TEST(lease.wait_for_holders(now()));
}

/**
 * @brief The last holder out wakes the closer
 */
BOOST_AUTO_TEST_CASE(test_the_last_holder_out_wakes_the_closer) {
    Lease lease{LeaseState::OPEN};
    BOOST_REQUIRE(lease.try_enter());
    BOOST_REQUIRE(lease.try_enter());
    std::thread holders([&] {
        std::this_thread::sleep_for(20ms);
        lease.leave();
        std::this_thread::sleep_for(20ms);
        lease.leave(); // the last out, with the closer parked
    });

    lease.state.exchange(LeaseState::CLOSING, std::memory_order_acq_rel);
    BOOST_TEST(lease.wait_for_holders(far()));
    BOOST_TEST(lease.active.load() == 0u);
    holders.join();
}

/**
 * @brief A holder that never leaves times the closer out, and the state is as the closer
 *        left it
 */
BOOST_AUTO_TEST_CASE(test_a_holder_that_never_leaves_times_the_closer_out) {
    Lease lease{LeaseState::OPEN};
    BOOST_REQUIRE(lease.try_enter());
    lease.state.exchange(LeaseState::CLOSING, std::memory_order_acq_rel);

    BOOST_TEST(!lease.wait_for_holders(Clock::now() + 30ms));
    BOOST_TEST((lease.state.load() == LeaseState::CLOSING));
    BOOST_TEST(lease.active.load() == 1u);
    lease.leave();
}

BOOST_AUTO_TEST_SUITE_END()
