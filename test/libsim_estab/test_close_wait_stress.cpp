/**
 * @file test_close_wait_stress.cpp
 * @brief Stress tests for the closer's park on the lease
 *
 * The closer parks until the last running holder leaves, and a holder leaves outside the
 * mutex: the one stated exception to the wake-up rule (design_engine_core.md §3.3, §5.1).
 * A lost wake-up is a hang, not a data race, so ThreadSanitizer does not report it. Each
 * test drives the park against holders that leave before, during and after the closer's
 * check, many times, and the CTest timeout is the failure. The ordering argument the park
 * rests on is checked by the litmus tests in test/litmus/, not here.
 *
 * sim_estab:engine.lease is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

module sim_estab;
import :engine.lease;

using namespace sim_estab::core::engine::lease;

namespace {

using Clock = Lease::Clock;

/// An `until` that no test reaches, so a lost wake-up hangs instead of expiring
Clock::time_point far() { return Clock::now() + std::chrono::hours(1); }

/// Every eighth round the holder leaves late, so the closer is parked when the leave lands.
/// In the other rounds the leave races the closer to the park.
void wait_if_late_round(std::uint64_t round) {
    if (round % 8 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(close_wait_stress_tests)

/**
 * @brief The closer wakes whenever the one holder leaves: before it parks, as it parks, or
 *        after
 */
BOOST_AUTO_TEST_CASE(test_the_last_holder_out_wakes_the_closer) {
    constexpr std::uint64_t rounds = 2'000;

    for (std::uint64_t round = 0; round < rounds; ++round) {
        Lease lease{LeaseState::OPEN};
        BOOST_REQUIRE(lease.try_enter());
        std::atomic<bool> go{false};
        std::thread holder([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            wait_if_late_round(round);
            lease.leave();
        });

        go.store(true, std::memory_order_release);
        lease.state.exchange(LeaseState::CLOSING, std::memory_order_acq_rel);
        BOOST_REQUIRE(lease.wait_for_holders(far()));
        BOOST_TEST(lease.active.load() == 0u);
        holder.join();
    }
}

/**
 * @brief With several holders, only the last one out wakes the closer, and it always does
 */
BOOST_AUTO_TEST_CASE(test_several_holders_leave_in_any_order) {
    constexpr std::uint64_t rounds       = 500;
    constexpr std::uint32_t holder_count = 4;

    for (std::uint64_t round = 0; round < rounds; ++round) {
        Lease lease{LeaseState::OPEN};
        for (std::uint32_t i = 0; i < holder_count; ++i) {
            BOOST_REQUIRE(lease.try_enter());
        }
        std::atomic<bool> go{false};
        std::vector<std::thread> holders;
        for (std::uint32_t i = 0; i < holder_count; ++i) {
            holders.emplace_back([&, i] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                wait_if_late_round(round + i);
                lease.leave();
            });
        }

        go.store(true, std::memory_order_release);
        lease.state.exchange(LeaseState::CLOSING, std::memory_order_acq_rel);
        BOOST_REQUIRE(lease.wait_for_holders(far()));
        BOOST_TEST(lease.active.load() == 0u);
        for (std::thread& holder : holders) {
            holder.join();
        }
    }
}

/**
 * @brief A holder that enters as the lease closes is either counted and waited for, or
 *        refused; the closer never returns with a holder inside
 */
BOOST_AUTO_TEST_CASE(test_a_holder_racing_the_close_is_counted_or_refused) {
    constexpr std::uint64_t rounds = 2'000;

    for (std::uint64_t round = 0; round < rounds; ++round) {
        Lease lease{LeaseState::OPEN};
        std::atomic<bool> go{false};
        std::atomic<bool> inside{false};
        std::atomic<bool> admitted{false};
        std::thread holder([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (lease.try_enter()) {
                admitted.store(true, std::memory_order_release);
                inside.store(true, std::memory_order_release);
                wait_if_late_round(round);
                inside.store(false, std::memory_order_release);
                lease.leave();
            }
        });

        go.store(true, std::memory_order_release);
        lease.state.exchange(LeaseState::CLOSING, std::memory_order_acq_rel);
        BOOST_REQUIRE(lease.wait_for_holders(far()));
        BOOST_TEST(!inside.load(std::memory_order_acquire)); // whoever was admitted has left
        holder.join();
        BOOST_TEST(lease.active.load() == 0u);
        (void)admitted; // either outcome is legal
    }
}

BOOST_AUTO_TEST_SUITE_END()
