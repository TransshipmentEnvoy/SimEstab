/**
 * @file sim_estab--engine.view.cppm
 * @brief Non-exported module partition for the engine view
 *
 * An engine view carries snapshots from the publisher to one reader: three blocks and one
 * atomic word (design_engine_core.md §3.2). The reader's return header travels back on the
 * same exchange (design_engine_core.md §3.5).
 *
 * The mechanism register row (design_engine_core.md §1.1):
 *
 * - Atomic objects: `word`, a 4-byte std::atomic<uint32_t>, always lock-free on x86-64.
 * - Memory orders: (P2) and (T2) exchange with acq_rel; (T1) loads relaxed. Nothing else
 *   is atomic.
 * - Linearization points: publish at (P2); take at (T2), which also publishes the return
 *   header.
 * - Progress: the publisher and the reader are wait-free, one exchange each.
 * - Failure: none. A due publish always has a writable block.
 *
 * This partition is internal: an importer of sim_estab cannot see it. Its tests are units
 * of the module (design_patterns.md §10).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

// Module declaration
module sim_estab:engine.view;

import :types;

/**
 * @namespace sim_estab::core::engine::view
 * @brief The engine view protocol and its return header
 */
namespace sim_estab::core::engine::view {

/// Size of PredicateParams in bytes. Provisional: the row predicate types that take
/// parameters arrive with projection (M5). It has room for six planes of four 32-bit
/// values, the largest type of design_engine_core.md §3.4.
inline constexpr std::size_t provisional_predicate_params_bytes = 96;

/**
 * One of an engine view's three payload buffers
 *
 * Until the world exists a block carries opaque bytes (design_engine_core.md §7 step 1).
 * The engine publishes none. Tests fill them, to check that a block arrives whole.
 */
struct Block {
    std::uint64_t tick = 0;               ///< the tick this block shows
    std::uint32_t size = 0;               ///< payload bytes in use
    std::unique_ptr<std::byte[]> payload; ///< `max_payload_bytes` bytes, allocated once
};

/// Row predicate parameters: a fixed size, interpreted by the predicate type
/// (design_engine_core.md §3.4). Opaque until the types beyond ALL exist (M5).
struct PredicateParams {
    std::array<std::byte, provisional_predicate_params_bytes> bytes{};
};

/**
 * The return header: written by the reader, read by the publisher
 * (design_engine_core.md §3.5)
 *
 * It may shape the engine view. It never reaches the world.
 */
struct ReturnHeader {
    std::uint32_t seq                = 0; ///< monotone; the publisher keeps the newest it has seen
    std::uint64_t last_consumed_tick = 0;
    std::uint32_t cadence_hint       = 0; ///< 0 = no preference
    PredicateParams params;
};

/**
 * The engine view (design_engine_core.md §3.2)
 *
 * Two roles use it, each from one thread at a time. The publisher fills its writable block
 * and calls publish(). The reader writes a return header and calls take(). A field marked
 * private to a role is touched by that role only.
 *
 * `{write_index, read_index, word >> 1}` is a permutation of `{0, 1, 2}` at every instant.
 * So the publisher never writes the block the reader holds.
 */
struct EngineView {
    std::unique_ptr<Block> block[3];
    std::atomic<std::uint32_t> word; ///< `(index << 1) | dirty`
    ReturnHeader ret[3];             ///< reader-written, parallel to block[]
    std::uint32_t write_index;       ///< publisher-private
    std::uint32_t read_index;        ///< reader-private

    ReturnHeader adopted;            ///< publisher-private: the newest return header it has seen
    std::uint32_t last_seq = 0;      ///< reader-private: seq of the last return header written
    std::uint32_t max_payload_bytes; ///< the most bytes a block's payload holds; fixed at construction

    /**
     * Allocate three blocks whose payloads hold `payload_bytes` bytes each
     *
     * The publisher starts with block 0 and the reader with block 1. Block 2 is in the
     * word, not dirty. The reader's block shows nothing until its first take succeeds.
     */
    explicit EngineView(std::uint32_t payload_bytes)
        : word(2u << 1), write_index(0), read_index(1), max_payload_bytes(payload_bytes) {
        for (std::unique_ptr<Block>& b : block) {
            b          = std::make_unique<Block>();
            b->payload = std::make_unique<std::byte[]>(max_payload_bytes);
        }
    }

    EngineView(const EngineView&)            = delete;
    EngineView& operator=(const EngineView&) = delete;

    /// Publisher: the block to fill before publish(). (P1)
    [[nodiscard]] Block& writable_block() noexcept { return *block[write_index]; }

    /**
     * Publisher: publish the writable block
     *
     * The block received in exchange brings its return header with it. The publisher adopts
     * that header if it is newer than the one it has (design_engine_core.md §3.5).
     */
    void publish() noexcept {
        const std::uint32_t prev   = word.exchange((write_index << 1) | 1u, std::memory_order_acq_rel); // (P2)
        write_index                = prev >> 1;
        const ReturnHeader& header = ret[write_index];
        if (header.seq > adopted.seq) {
            adopted = header;
        }
    }

    /// Reader: write the return header that the next successful take() hands to the
    /// publisher. Call it before take() (design_engine_core.md §3.5).
    void write_return(types::Tick last_consumed_tick, std::uint32_t cadence_hint,
                      const PredicateParams& params) noexcept {
        ReturnHeader& header      = ret[read_index];
        header.seq                = ++last_seq;
        header.last_consumed_tick = last_consumed_tick.value;
        header.cadence_hint       = cadence_hint;
        header.params             = params;
    }

    /**
     * Reader: take the newest published block, handing back the one held
     *
     * @return true if a newer block was taken. false if nothing new was published: the
     *         reader keeps the block it holds (the pseudo-code's HELD).
     */
    [[nodiscard]] bool take() noexcept {
        if ((word.load(std::memory_order_relaxed) & 1u) == 0) { // (T1)
            return false;
        }
        const std::uint32_t prev = word.exchange(read_index << 1, std::memory_order_acq_rel); // (T2)
        read_index               = prev >> 1;
        return true;
    }

    /// Reader: the block it holds. It shows a snapshot once the first take() has succeeded.
    [[nodiscard]] const Block& held_block() const noexcept { return *block[read_index]; }
};

} // namespace sim_estab::core::engine::view
