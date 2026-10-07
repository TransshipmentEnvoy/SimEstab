module;

// compat
#include "compat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <mutex>
#include <vector>

module sim_estab;
import :engine.event_ring;

using namespace sim_estab::core::log;

namespace sim_estab::core::engine::event_ring {

namespace {

[[noreturn]] void abort_with(const char *what) noexcept {
    sim_estab_log("sim_estab.engine.event_ring", severity_level::critical, "programming error: ", what);
    std::abort();
}

} // namespace

void EventRing::abort_when_full() noexcept {
    abort_with("enqueue into a full event ring; the gate must stop the executor at high_water");
}

void EventRing::drain(std::vector<EngineEvent>& out) {
    if (event_drain != EventDrain::Owner) {
        abort_with("drain() under the sink drain; the executor empties the ring itself");
    }

    std::list<EngineEvent> outside;
    {
        std::lock_guard lk(side_m);
        outside.swap(side_list);
    }
    const std::uint64_t first = read.load(std::memory_order_relaxed); // our own word
    const std::uint64_t end   = write.load(std::memory_order_acquire);
    try {
        out.reserve(out.size() + static_cast<std::size_t>(end - first) + outside.size());
    } catch (...) {
        // Nothing is lost: the side events go back ahead of any raised since.
        std::lock_guard lk(side_m);
        side_list.splice(side_list.begin(), outside);
        throw;
    }

    // The ring is in tick order already. An event raised outside a tick goes before the ring
    // events of its tick (design_engine_core.md §5.2). The sort is stable, so events of one
    // tick keep the order they were raised in.
    outside.sort([](const EngineEvent& a, const EngineEvent& b) { return a.tick < b.tick; });
    auto side = outside.begin();
    for (std::uint64_t index = first; index != end; ++index) {
        const EngineEvent& event = slot[index % size];
        for (; side != outside.end() && side->tick <= event.tick; ++side) {
            out.push_back(*side);
        }
        out.push_back(event);
    }
    out.insert(out.end(), side, outside.end());

    read.store(end, std::memory_order_release); // the slots may be written again
}

void EventRing::drain_to_sink() {
    std::list<EngineEvent> outside;
    {
        std::lock_guard lk(side_m);
        outside.swap(side_list);
    }
    const std::uint64_t first = read.load(std::memory_order_relaxed);
    const std::uint64_t end   = write.load(std::memory_order_relaxed); // the executor's own word
    sink_count += (end - first) + outside.size();
    read.store(end, std::memory_order_release);
}

} // namespace sim_estab::core::engine::event_ring
