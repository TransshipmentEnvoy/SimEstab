/**
 * @file test_engine_view_tsan.cpp
 * @brief Race tests for the engine view protocol
 *
 * A publisher thread and a reader thread run the protocol against each other. These tests
 * prove something only in a ThreadSanitizer build: every wrongly ordered variant of the
 * protocol still computes correct results on x86-64 (design_engine_core.md §3.2).
 *
 * They cover the forward edge (a taken block is whole), the reverse edge (the publisher
 * reuses a block only after the reader has finished with it), and the return header that
 * rides the reverse edge (design_engine_core.md §3.5).
 *
 * sim_estab:engine_view is not exported, so this file is a unit of the module
 * (design_patterns.md §10).
 */

module;

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

module sim_estab;
import :engine_view;

using namespace sim_estab::core::engine_view;

namespace {

constexpr std::uint32_t payload_bytes = 64;

/// Each test publishes at least this often, and goes on until the two threads have met at
/// least `min_handovers` times. So a reader that starts late cannot make a test pass empty.
constexpr std::uint64_t min_publishes = 100'000;
constexpr std::uint64_t min_handovers = 1'000;

/// The byte a whole block of `tick` holds at offset `i`
std::byte pattern(std::uint64_t tick, std::uint32_t i) { return static_cast<std::byte>((tick + i) & 0xFF); }

/// Publisher: fill the whole block for `tick`
void fill(Block& block, std::uint64_t tick) {
    block.tick = tick;
    block.size = payload_bytes;
    for (std::uint32_t i = 0; i < payload_bytes; ++i) {
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

} // namespace

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(engine_view_tsan_tests)

/**
 * @brief Every taken block is whole, ticks only go forward, and a held block stays as it is
 *
 * The reader reads all of its block after every take attempt, also when the take held. The
 * publisher fills every block it receives. A publisher that wrote the reader's block, or
 * reused a block before the reader had finished with it, is a data race here.
 */
BOOST_AUTO_TEST_CASE(test_blocks_arrive_whole_and_in_order) {
    EngineView view{payload_bytes};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> taken{0};

    // owned by the reader thread until it is joined
    std::uint64_t torn         = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t last_tick    = 0;

    std::thread reader([&] {
        const PredicateParams params{};
        for (;;) {
            const bool finished = done.load(std::memory_order_acquire);
            view.write_return(last_tick, 0, params);
            if (view.take()) {
                const Block& held = view.held_block();
                if (!is_whole(held)) {
                    ++torn;
                }
                if (held.tick <= last_tick) {
                    ++out_of_order;
                }
                last_tick = held.tick;
                taken.fetch_add(1, std::memory_order_relaxed);
            } else if (last_tick != 0 && !is_whole(view.held_block())) {
                ++torn;
            }
            if (finished) {
                break; // this attempt came after the last publish
            }
        }
    });

    std::uint64_t tick = 0;
    while (tick < min_publishes || taken.load(std::memory_order_relaxed) < min_handovers) {
        fill(view.writable_block(), ++tick);
        view.publish();
    }
    done.store(true, std::memory_order_release);
    reader.join();

    BOOST_TEST(torn == 0u);
    BOOST_TEST(out_of_order == 0u);
    BOOST_TEST(last_tick == tick);
    BOOST_TEST(taken.load(std::memory_order_relaxed) >= min_handovers);
    BOOST_TEST(is_permutation(view));
}

/**
 * @brief A return header written before a take reaches the publisher whole, and never an
 *        older one after a newer one
 *
 * Every field of a header follows from its seq, so the publisher can tell a torn header
 * from a whole one. A publisher that read a header while the reader was still writing it
 * is a data race here.
 */
BOOST_AUTO_TEST_CASE(test_return_headers_arrive_whole) {
    EngineView view{payload_bytes};
    std::atomic<bool> done{false};

    std::thread reader([&] {
        std::uint64_t last_tick = 0;
        for (;;) {
            const bool finished     = done.load(std::memory_order_acquire);
            const std::uint32_t seq = view.last_seq + 1;
            PredicateParams params;
            params.bytes.fill(static_cast<std::byte>(seq & 0xFF));
            view.write_return(last_tick, seq, params);
            if (view.take()) {
                last_tick = view.held_block().tick;
            }
            if (finished) {
                break;
            }
        }
    });

    // owned by the publisher, which is this thread
    std::uint64_t torn             = 0;
    std::uint64_t went_back        = 0;
    std::uint64_t from_the_future  = 0;
    std::uint64_t adoptions        = 0;
    std::uint32_t last_adopted_seq = 0;

    std::uint64_t tick = 0;
    while (tick < min_publishes || adoptions < min_handovers) {
        fill(view.writable_block(), ++tick);
        view.publish();

        const ReturnHeader& adopted = view.adopted;
        if (adopted.seq < last_adopted_seq) {
            ++went_back;
        }
        if (adopted.seq != last_adopted_seq) {
            ++adoptions;
            const std::byte expected = static_cast<std::byte>(adopted.seq & 0xFF);
            const bool whole         = adopted.cadence_hint == adopted.seq &&
                               std::ranges::all_of(adopted.params.bytes, [&](std::byte b) { return b == expected; });
            if (!whole) {
                ++torn;
            }
            if (adopted.last_consumed_tick >= tick) {
                ++from_the_future; // the reader cannot have consumed the tick being published
            }
            last_adopted_seq = adopted.seq;
        }
    }
    done.store(true, std::memory_order_release);
    reader.join();

    BOOST_TEST(torn == 0u);
    BOOST_TEST(went_back == 0u);
    BOOST_TEST(from_the_future == 0u);
    BOOST_TEST(adoptions >= min_handovers);
    BOOST_TEST(is_permutation(view));
}

BOOST_AUTO_TEST_SUITE_END()
