/**
 * @file test_engine_view.cpp
 * @brief Tests for the engine view protocol on one thread
 *
 * These check what each step does: the permutation of the three block indices, what a take
 * returns, and which return header the publisher adopts (design_engine_core.md §3.2, §3.5).
 * They cannot tell a correctly ordered implementation from a wrongly ordered one. The race
 * tests in test_engine_view_tsan.cpp do that.
 *
 * sim_estab:engine.view is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

module sim_estab;
import :engine.view;

using namespace sim_estab::core::engine::view;

namespace {

constexpr std::uint32_t payload_bytes = 16;

/// The byte a whole block of `tick` holds at offset `i`
std::byte pattern(std::uint64_t tick, std::uint32_t i) { return static_cast<std::byte>((tick + i) & 0xFF); }

/// Publisher: fill the whole block for `tick`
void fill(Block& block, std::uint64_t tick, std::uint32_t bytes) {
    block.tick = tick;
    block.size = bytes;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        block.payload[i] = pattern(tick, i);
    }
}

/// Reader: true if every byte of the block belongs to the tick it shows
bool is_whole(const Block& block) {
    for (std::uint32_t i = 0; i < block.size; ++i) {
        if (block.payload[i] != pattern(block.tick, i)) {
            return false;
        }
    }
    return true;
}

/// True if the publisher's index, the reader's index and the index in the word are 0, 1 and
/// 2 in some order. Call it only while no other thread uses the engine view.
bool is_permutation(const EngineView& view) {
    std::array<std::uint32_t, 3> indices{view.write_index, view.read_index,
                                         view.word.load(std::memory_order_relaxed) >> 1};
    std::ranges::sort(indices);
    return indices == std::array<std::uint32_t, 3>{0, 1, 2};
}

void publish(EngineView& view, std::uint64_t tick) {
    fill(view.writable_block(), tick, payload_bytes);
    view.publish();
}

PredicateParams params_of(std::byte value) {
    PredicateParams params;
    params.bytes.fill(value);
    return params;
}

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_view_tests)

/**
 * @brief A new engine view holds a permutation, and has nothing to take
 */
BOOST_AUTO_TEST_CASE(test_starts_as_a_permutation_with_nothing_published) {
    EngineView view{payload_bytes};

    BOOST_TEST(is_permutation(view));
    BOOST_TEST(view.max_payload_bytes == payload_bytes);
    BOOST_TEST((view.word.load(std::memory_order_relaxed) & 1u) == 0u);
    BOOST_TEST(!view.take());
    BOOST_TEST(is_permutation(view));
}

/**
 * @brief A take returns the published block, and the publisher gets another one
 */
BOOST_AUTO_TEST_CASE(test_take_returns_the_published_block) {
    EngineView view{payload_bytes};

    publish(view, 1);
    BOOST_TEST(is_permutation(view));

    BOOST_TEST(view.take());
    const Block& held = view.held_block();
    BOOST_TEST(held.tick == 1u);
    BOOST_TEST(held.size == payload_bytes);
    BOOST_TEST(is_whole(held));
    BOOST_TEST(&view.writable_block() != &held);
    BOOST_TEST(is_permutation(view));
}

/**
 * @brief A take with nothing new keeps the block the reader holds
 */
BOOST_AUTO_TEST_CASE(test_take_holds_when_nothing_new_is_published) {
    EngineView view{payload_bytes};

    publish(view, 1);
    BOOST_TEST(view.take());
    const Block *held = &view.held_block();

    BOOST_TEST(!view.take());
    BOOST_TEST(&view.held_block() == held);
    BOOST_TEST(view.held_block().tick == 1u);
    BOOST_TEST(is_permutation(view));
}

/**
 * @brief An engine view is sampled: a publish nobody took is overwritten
 */
BOOST_AUTO_TEST_CASE(test_an_untaken_publish_is_overwritten) {
    EngineView view{payload_bytes};

    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        publish(view, tick);
        BOOST_TEST(is_permutation(view));
    }

    BOOST_TEST(view.take());
    BOOST_TEST(view.held_block().tick == 3u);
    BOOST_TEST(is_whole(view.held_block()));
    BOOST_TEST(!view.take());
}

/**
 * @brief The publisher never writes the block the reader holds
 */
BOOST_AUTO_TEST_CASE(test_the_reader_keeps_its_block_across_publishes) {
    EngineView view{payload_bytes};

    publish(view, 1);
    BOOST_TEST(view.take());
    const Block *held = &view.held_block();

    for (std::uint64_t tick = 2; tick <= 6; ++tick) {
        BOOST_TEST(&view.writable_block() != held);
        publish(view, tick);
        BOOST_TEST(is_permutation(view));
    }

    BOOST_TEST(&view.held_block() == held);
    BOOST_TEST(held->tick == 1u);
    BOOST_TEST(is_whole(*held));
}

/**
 * @brief A return header written before a take reaches the publisher with the block
 */
BOOST_AUTO_TEST_CASE(test_the_publisher_adopts_a_return_header) {
    EngineView view{payload_bytes};

    publish(view, 1);
    BOOST_TEST(view.adopted.seq == 0u);

    view.write_return(0, 5, params_of(std::byte{0xA1}));
    BOOST_TEST(view.take());
    BOOST_TEST(view.adopted.seq == 0u); // the block has not come back yet

    publish(view, 2);
    BOOST_TEST(view.adopted.seq == 1u);
    BOOST_TEST(view.adopted.last_consumed_tick == 0u);
    BOOST_TEST(view.adopted.cadence_hint == 5u);
    BOOST_TEST((view.adopted.params.bytes == params_of(std::byte{0xA1}).bytes));
}

/**
 * @brief The publisher adopts only a newer return header
 *
 * Blocks come back in an order that hands the publisher an older header after a newer one.
 */
BOOST_AUTO_TEST_CASE(test_the_publisher_keeps_the_newest_return_header) {
    EngineView view{payload_bytes};

    publish(view, 1);
    view.write_return(0, 5, params_of(std::byte{0xA1}));
    BOOST_TEST(view.take());

    publish(view, 2);
    BOOST_TEST(view.adopted.seq == 1u);

    view.write_return(1, 7, params_of(std::byte{0xB2}));
    BOOST_TEST(view.take());

    publish(view, 3);
    BOOST_TEST(view.adopted.seq == 2u);
    BOOST_TEST(view.adopted.cadence_hint == 7u);

    publish(view, 4); // brings back the block that carries the first header
    BOOST_TEST(view.adopted.seq == 2u);
    BOOST_TEST(view.adopted.last_consumed_tick == 1u);
    BOOST_TEST(view.adopted.cadence_hint == 7u);
    BOOST_TEST((view.adopted.params.bytes == params_of(std::byte{0xB2}).bytes));
}

/**
 * @brief A return header reaches the publisher only through a take
 */
BOOST_AUTO_TEST_CASE(test_a_return_header_without_a_take_stays_with_the_reader) {
    EngineView view{payload_bytes};

    view.write_return(0, 5, params_of(std::byte{0xA1}));
    BOOST_TEST(!view.take());

    publish(view, 1);
    BOOST_TEST(view.adopted.seq == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
