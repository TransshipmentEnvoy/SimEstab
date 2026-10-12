/**
 * @file test_engine_endpoint.cpp
 * @brief Tests for command endpoints
 *
 * These check the ring depths, every rejection a submit can return, the order of the drain,
 * what the drain does with commands for a later or a past tick, every outcome, the endpoint
 * lease and the deadlock rule (design_engine_core.md §5.1). They run on one thread;
 * test_endpoint_tsan.cpp runs endpoints across threads.
 *
 * The mechanism of sim_estab:engine.endpoint is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

module sim_estab;
import :engine.endpoint;
import :engine.event_ring;
import :engine.gate;
import :engine.lease;

using namespace sim_estab::core::engine::endpoint;
using sim_estab::core::engine::event_ring::EngineEvent;
using sim_estab::core::engine::event_ring::EngineEventType;
using sim_estab::core::engine::event_ring::EventDrain;
using sim_estab::core::engine::event_ring::EventRing;
using sim_estab::core::engine::gate::TickGate;
using sim_estab::core::engine::lease::LeaseState;
using sim_estab::core::engine::tick::Tick;

namespace {

using Clock = Endpoint::Clock;

/// The first tick a new session runs: the freeze has published tick 0
constexpr Tick first_tick{1};

/// A command whose payload starts with `mark`
Command marked(std::uint64_t mark) {
    Command command;
    for (std::size_t i = 0; i < sizeof mark; ++i) {
        command.payload[i] = static_cast<std::byte>(mark >> (8 * i));
    }
    return command;
}

/// The mark marked() wrote
std::uint64_t mark_of(const Command& command) {
    std::uint64_t mark = 0;
    for (std::size_t i = 0; i < sizeof mark; ++i) {
        mark |= std::uint64_t{std::to_integer<std::uint8_t>(command.payload[i])} << (8 * i);
    }
    return mark;
}

/// One command as the drain emitted it
struct Drained {
    std::uint32_t source;
    std::uint64_t mark;
    std::uint64_t sequence;

    bool operator==(const Drained&) const = default;
};

/// Drain `endpoints` at `tick`, as the executor does at the start of a tick
std::vector<Drained> drain(std::span<Endpoint *const> endpoints, Tick tick) {
    std::vector<Drained> out;
    drain_commands(endpoints, tick, [&](const Command& command, std::uint32_t source, std::uint64_t sequence) {
        out.push_back({source, mark_of(command), sequence});
    });
    return out;
}

/// Drain one endpoint at `tick`
std::vector<Drained> drain(Endpoint& endpoint, Tick tick) {
    std::array<Endpoint *, 1> one{&endpoint};
    return drain(one, tick);
}

/// Run `tick` for one endpoint: drain it, then finish the tick (G5)
std::vector<Drained> run_tick(Endpoint& endpoint, Tick tick) {
    std::vector<Drained> out = drain(endpoint, tick);
    endpoint.tick_gate.finish_tick(tick);
    return out;
}

/// Every event drained so far
std::vector<EngineEvent> drained_events(EventRing& events) {
    std::vector<EngineEvent> out;
    events.drain(out);
    return out;
}

CommandOutcome pending(Tick tick) { return {CommandOutcomeType::Pending, tick}; }
CommandOutcome applied(Tick tick) { return {CommandOutcomeType::Applied, tick}; }

/// An `until` already passed: wait_outcome() checks once and returns
Clock::time_point now() { return Clock::now(); }

/// An `until` that no test reaches
Clock::time_point far() { return Clock::now() + std::chrono::hours(1); }

/// A session with one participant, the paced producer's
struct Session {
    EventRing events{64, 8, EventDrain::Owner};
    TickGate tick_gate{1, first_tick, events};
};

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_endpoint_tests)

/**
 * @brief A ring is one tick deep, or two for a paced endpoint with a margin
 */
BOOST_AUTO_TEST_CASE(test_the_depth_follows_capacity_and_margin) {
    Session session;
    Endpoint host{session.tick_gate, 0, 4};
    Endpoint engine_source{session.tick_gate, 1, 4, 0, 0};
    Endpoint peer{session.tick_gate, 2, 4, 0, 3};

    BOOST_TEST(host.ring.depth == 4u);
    BOOST_TEST(engine_source.ring.depth == 4u);
    BOOST_TEST(peer.ring.depth == 8u);
    BOOST_TEST(host.unpaced);
    BOOST_TEST(!peer.unpaced);
}

/**
 * @brief An endpoint admits nothing until it is opened
 */
BOOST_AUTO_TEST_CASE(test_a_new_endpoint_is_revoked) {
    Session session;
    Endpoint host{session.tick_gate, 0, 4};

    BOOST_TEST((host.lease.state.load() == LeaseState::CLOSED));
    BOOST_TEST((host.submit(marked(1)) == std::unexpected(Rejection::Revoked)));
    BOOST_TEST(host.ring.write.load() == 0u);
}

/**
 * @brief An unpaced submit never waits: past `capacity` commands it returns QueueFull, and
 *        the next drain takes them all
 */
BOOST_AUTO_TEST_CASE(test_an_unpaced_endpoint_fills_and_the_drain_empties_it) {
    Session session;
    Endpoint host{session.tick_gate, 0, 3};
    host.open();

    for (std::uint64_t i = 0; i < 3; ++i) {
        BOOST_TEST((host.submit(marked(i)) == CommandHandle{0, i}));
    }
    BOOST_TEST((host.submit(marked(9)) == std::unexpected(Rejection::QueueFull)));

    BOOST_TEST((run_tick(host, first_tick) == std::vector<Drained>{{0, 0, 0}, {0, 1, 1}, {0, 2, 2}}));
    BOOST_TEST((host.submit(marked(3)) == CommandHandle{0, 3}));
    BOOST_TEST((run_tick(host, first_tick + 1) == std::vector<Drained>{{0, 3, 0}}));
}

/**
 * @brief The drain takes endpoints in ascending source id, each in FIFO order, and
 *        numbers the commands by their place in the drain
 */
BOOST_AUTO_TEST_CASE(test_the_drain_orders_by_source_then_submission) {
    Session session;
    Endpoint host{session.tick_gate, 0, 4};
    Endpoint mod{session.tick_gate, 3, 4};
    Endpoint other_mod{session.tick_gate, 7, 4};
    for (Endpoint *endpoint : {&host, &mod, &other_mod}) {
        endpoint->open();
    }

    // submitted interleaved; drained by source
    (void)other_mod.submit(marked(70));
    (void)host.submit(marked(1));
    (void)mod.submit(marked(30));
    (void)host.submit(marked(2));
    (void)other_mod.submit(marked(71));

    std::array<Endpoint *, 3> endpoints{&host, &mod, &other_mod};
    BOOST_TEST((drain(endpoints, first_tick) ==
                std::vector<Drained>{{0, 1, 0}, {0, 2, 1}, {3, 30, 2}, {7, 70, 3}, {7, 71, 4}}));
}

/**
 * @brief Each rejection of a paced submit: a past or released tick, an earlier stamp, a tick
 *        beyond the margin, and more than `capacity` commands for one tick
 */
BOOST_AUTO_TEST_CASE(test_a_paced_submit_checks_its_stamp) {
    Session session;
    Endpoint engine_source{session.tick_gate, 1, 2, 0, 0};
    engine_source.open();

    BOOST_TEST((engine_source.submit(marked(0), first_tick - 1) == std::unexpected(Rejection::TooLate)));
    BOOST_TEST((engine_source.submit(marked(0), first_tick + 1) == std::unexpected(Rejection::OverMargin)));
    BOOST_TEST(engine_source.submit(marked(1), first_tick).has_value());
    BOOST_TEST(engine_source.submit(marked(2), first_tick).has_value());
    BOOST_TEST((engine_source.submit(marked(3), first_tick) == std::unexpected(Rejection::QueueFull)));

    session.tick_gate.declare_ready(0, first_tick);
    BOOST_TEST((engine_source.submit(marked(4), first_tick) == std::unexpected(Rejection::TooLate)));
    BOOST_TEST(engine_source.ring.write.load() == 2u);
}

/**
 * @brief A producer with a margin may stamp ahead, but never backwards, and never past its
 *        margin
 */
BOOST_AUTO_TEST_CASE(test_a_margin_lets_a_producer_stamp_ahead) {
    Session session;
    Endpoint peer{session.tick_gate, 2, 2, 0, 2};
    peer.open();

    BOOST_TEST(peer.submit(marked(1), first_tick + 2).has_value());
    BOOST_TEST((peer.submit(marked(2), first_tick + 1) == std::unexpected(Rejection::OutOfOrder)));
    BOOST_TEST((peer.submit(marked(3), first_tick + 3) == std::unexpected(Rejection::OverMargin)));
    BOOST_TEST(peer.submit(marked(4), first_tick + 2).has_value());
    BOOST_TEST((peer.submit(marked(5), first_tick + 2) == std::unexpected(Rejection::QueueFull)));
}

/**
 * @brief A command for a later tick keeps its place, with everything behind it, until the
 *        drain of its tick
 */
BOOST_AUTO_TEST_CASE(test_the_drain_leaves_later_ticks_in_place) {
    Session session;
    Endpoint peer{session.tick_gate, 2, 2, 0, 2};
    peer.open();

    (void)peer.submit(marked(1), first_tick);
    (void)peer.submit(marked(2), first_tick + 1);
    (void)peer.submit(marked(3), first_tick + 1);

    BOOST_TEST((run_tick(peer, first_tick) == std::vector<Drained>{{2, 1, 0}}));
    BOOST_TEST((run_tick(peer, first_tick + 1) == std::vector<Drained>{{2, 2, 0}, {2, 3, 1}}));
    BOOST_TEST(peer.ring.read.load() == 3u);
}

/**
 * @brief A command whose tick has passed is dropped, never run late, and the drain reports
 *        it once for its endpoint (S3)
 */
BOOST_AUTO_TEST_CASE(test_a_command_for_a_past_tick_is_dropped) {
    Session session;
    Endpoint peer{session.tick_gate, 2, 2, 0, 1};
    peer.open();

    const auto first  = peer.submit(marked(1), first_tick);
    const auto second = peer.submit(marked(2), first_tick);
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE(second.has_value());

    // The tick passes without its drain, as when the gate stopped waiting for this producer
    // between its check and its publish.
    session.tick_gate.finish_tick(first_tick);
    BOOST_TEST(drain(peer, first_tick + 1).empty());
    BOOST_TEST((drained_events(session.events) ==
                std::vector<EngineEvent>{{(first_tick + 1).value, EngineEventType::CommandProtocolError, 2, 2}}));

    BOOST_TEST((peer.outcome(*first) == pending(first_tick + 1)));
    session.tick_gate.finish_tick(first_tick + 1);
    BOOST_TEST((peer.outcome(*first) == CommandOutcome{CommandOutcomeType::Dropped, first_tick + 1}));
    BOOST_TEST((peer.outcome(*second) == CommandOutcome{CommandOutcomeType::Dropped, first_tick + 1}));
}

/**
 * @brief An unpaced command's outcome is Pending until the tick that drained it has run,
 *        then Applied with that tick
 */
BOOST_AUTO_TEST_CASE(test_an_unpaced_outcome_reports_the_tick_that_ran_it) {
    Session session;
    Endpoint host{session.tick_gate, 0, 2};
    host.open();
    session.tick_gate.finish_tick(first_tick); // a tick with nothing to drain

    const auto handle = host.submit(marked(1));
    BOOST_REQUIRE(handle.has_value());
    BOOST_TEST((host.outcome(*handle) == pending(Tick{0})));

    BOOST_TEST(drain(host, first_tick + 1).size() == 1u);
    BOOST_TEST((host.outcome(*handle) == pending(first_tick + 1))); // drained, not run yet

    session.tick_gate.finish_tick(first_tick + 1);
    BOOST_TEST((host.outcome(*handle) == applied(first_tick + 1)));
}

/**
 * @brief A paced command's outcome is Pending with the tick it names, then Applied
 */
BOOST_AUTO_TEST_CASE(test_a_paced_outcome_reports_its_tick) {
    Session session;
    Endpoint peer{session.tick_gate, 2, 2, 0, 1};
    peer.open();

    const auto handle = peer.submit(marked(1), first_tick + 1);
    BOOST_REQUIRE(handle.has_value());
    BOOST_TEST((peer.outcome(*handle) == pending(first_tick + 1)));

    BOOST_TEST(run_tick(peer, first_tick).empty());
    BOOST_TEST(run_tick(peer, first_tick + 1).size() == 1u);
    BOOST_TEST((peer.outcome(*handle) == applied(first_tick + 1)));
}

/**
 * @brief A handle reads Expired once its producer has submitted `depth` commands after it,
 *        and not before
 */
BOOST_AUTO_TEST_CASE(test_a_handle_expires_after_depth_more_submits) {
    Session session;
    Endpoint host{session.tick_gate, 0, 2};
    host.open();

    const auto oldest = host.submit(marked(0));
    const auto middle = host.submit(marked(1));
    BOOST_REQUIRE(oldest.has_value());
    BOOST_REQUIRE(middle.has_value());
    (void)run_tick(host, first_tick);
    BOOST_TEST((host.outcome(*oldest) == applied(first_tick)));

    const auto newest = host.submit(marked(2));
    BOOST_REQUIRE(newest.has_value());
    BOOST_TEST((host.outcome(*oldest) == CommandOutcome{CommandOutcomeType::Expired, Tick{0}}));
    BOOST_TEST((host.outcome(*middle) == applied(first_tick)));
}

/**
 * @brief Revocation refuses new submits but keeps what was admitted, and a reopened
 *        endpoint drains its old commands before new ones
 */
BOOST_AUTO_TEST_CASE(test_a_revoked_endpoint_keeps_its_admitted_commands) {
    Session session;
    Endpoint host{session.tick_gate, 0, 4};
    host.open();

    (void)host.submit(marked(1));
    BOOST_TEST(host.revoke(far()));
    BOOST_TEST((host.lease.state.load() == LeaseState::CLOSED));
    BOOST_TEST((host.submit(marked(2)) == std::unexpected(Rejection::Revoked)));

    host.open();
    (void)host.submit(marked(3));
    BOOST_TEST((run_tick(host, first_tick) == std::vector<Drained>{{0, 1, 0}, {0, 3, 1}}));
}

/**
 * @brief A paced producer may wait only for the outcome of a tick it has released
 */
BOOST_AUTO_TEST_CASE(test_an_outcome_wait_refuses_an_unreleased_tick) {
    Session session;
    Endpoint peer{session.tick_gate, 2, 2, 0, 1};
    peer.open();

    const auto handle = peer.submit(marked(1), first_tick);
    BOOST_REQUIRE(handle.has_value());
    BOOST_TEST((peer.wait_outcome(*handle, far()) == std::unexpected(OutcomeWaitRefusal::Unreleased)));

    session.tick_gate.declare_ready(0, first_tick);
    BOOST_TEST((peer.wait_outcome(*handle, now()) == pending(first_tick))); // `until` has passed

    (void)run_tick(peer, first_tick);
    BOOST_TEST((peer.wait_outcome(*handle, far()) == applied(first_tick)));
}

/**
 * @brief An outcome wait on a revoked endpoint returns Revoked while the command is pending
 */
BOOST_AUTO_TEST_CASE(test_an_outcome_wait_on_a_revoked_endpoint_returns) {
    Session session;
    Endpoint host{session.tick_gate, 0, 2};
    host.open();

    const auto handle = host.submit(marked(1));
    BOOST_REQUIRE(handle.has_value());
    BOOST_TEST(host.revoke(far()));
    BOOST_TEST((host.wait_outcome(*handle, far()) == std::unexpected(OutcomeWaitRefusal::Revoked)));

    (void)run_tick(host, first_tick); // admitted before the revoke: it still runs
    BOOST_TEST((host.wait_outcome(*handle, far()) == applied(first_tick)));
}

BOOST_AUTO_TEST_SUITE_END()
