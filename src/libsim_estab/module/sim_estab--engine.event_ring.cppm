/**
 * @file sim_estab--engine.event_ring.cppm
 * @brief Non-exported module partition for the event ring
 *
 * The event ring carries events from the executor to the session's event drain
 * (design_engine_core.md §5.2). Only the engine raises events, about itself: the session, the
 * gate, participants, endpoints and engine views. A change in the world is never an event.
 * Events raised outside a tick wait in the side list, and the drain merges them in.
 *
 * Until M2 defines the event payload, an event is a fixed record (design_engine_core.md §7
 * step 1), and the sink counts events without hashing them (open_question.md Q85).
 *
 * The mechanism register row (design_engine_core.md §1.1):
 *
 * - Atomic objects: `write` and `read`, 8 bytes each, always lock-free on x86-64.
 * - Memory orders: enqueue fills the slot, then stores `write` with release; the drain loads
 *   it with acquire. The drain stores `read` with release; the executor loads it with
 *   acquire, so it writes a slot again only after the drain has read it.
 * - Linearization point: enqueue, at the release store of `write`.
 * - Progress: enqueue and both drains are wait-free. The side list takes a short lock.
 * - Failure: none. The gate stops the executor at `high_water`, so the ring cannot overflow.
 *   An enqueue into a full ring is a programming error, and aborts.
 *
 * This partition is internal: an importer of sim_estab cannot see it. Its tests are units
 * of the module (design_patterns.md §10).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

// Module declaration
module sim_estab:engine.event_ring;

/**
 * @namespace sim_estab::core::engine::event_ring
 * @brief The event ring, its side list and its two drains
 */
namespace sim_estab::core::engine::event_ring {

/// What an event reports (design_python_api.md §7.3). The comment gives each one's topic.
enum class EngineEventType : std::uint32_t {
    SessionStopped,     ///< session.stopped: the executor met the stop at the gate; no tick runs again
    SimBacklogPaused,   ///< sim.backlog_paused: the gate stopped at high_water
    SimBacklogResumed,  ///< sim.backlog_resumed: a tick passed the gate again below high_water
    ParticipantExpired, ///< participant.expired: a participant's deadline passed; subject is the participant
};

/**
 * One event (design_engine_core.md §5.2)
 *
 * A fixed record until M2 defines the event payload (design_engine_core.md §7 step 1).
 */
struct EngineEvent {
    std::uint64_t tick    = 0; ///< the tick running when it was raised; between ticks, the next one to run
    EngineEventType type  = EngineEventType::SessionStopped;
    std::uint32_t subject = 0; ///< what it is about: a participant index, a source id or an engine view index
    std::uint64_t count   = 0; ///< a number its type defines; 0 if none

    bool operator==(const EngineEvent&) const = default;
};

/// Who empties the event ring: `EngineConfig.event_drain` (design_python_api.md §3)
enum class EventDrain : std::uint32_t {
    Owner, ///< the owner thread, with drain_events(), between ticks
    Sink,  ///< the executor, after every tick and when a step ends; it counts and keeps nothing
};

/**
 * E: the most events the engine raises into the ring in one tick (design_engine_core.md §5.2)
 *
 * One cap refusal per object type and source, one protocol-error report per source, three
 * participant changes per participant, one bandwidth warning per engine view, and four
 * session-wide events.
 */
[[nodiscard]] constexpr std::uint64_t events_per_tick(std::uint64_t object_types, std::uint64_t sources,
                                                      std::uint64_t participants, std::uint64_t engine_views) noexcept {
    return object_types * sources + sources + 3 * participants + engine_views + 4;
}

/**
 * The event ring (design_engine_core.md §5.2)
 *
 * The executor enqueues, and the session's event drain empties the ring. Any thread may
 * raise an event outside a tick into the side list. A field marked private to a role is
 * touched by that role only.
 *
 * The ring holds E x (D + 1) events, and the gate stops the executor at E x D. A tick that
 * passes just below that mark adds at most E, so the ring cannot overflow.
 */
struct EventRing {
    std::unique_ptr<EngineEvent[]> slot; ///< `size` events
    std::uint64_t size;                  ///< E x (D + 1)
    std::uint64_t high_water;            ///< E x D: the backlog at which the gate blocks
    EventDrain event_drain;              ///< fixed at construction
    std::atomic<std::uint64_t> write;    ///< executor-written: events enqueued so far
    std::atomic<std::uint64_t> read;     ///< drain-written: events consumed so far
    std::uint64_t sink_count = 0;        ///< executor-private: events the sink has consumed

    /// The side list: events raised outside a tick. A list, so that the lock is held only to
    /// link or swap nodes, never to allocate.
    std::mutex side_m;
    std::list<EngineEvent> side_list; ///< guarded by side_m

    /**
     * A ring for `per_tick` events per tick (E, from events_per_tick()), sized for
     * `drain_interval_ticks` ticks between two drains (D, design_limits.md §2)
     */
    EventRing(std::uint64_t per_tick, std::uint32_t drain_interval_ticks, EventDrain drained_by)
        : slot(std::make_unique<EngineEvent[]>(per_tick * (std::uint64_t{drain_interval_ticks} + 1))),
          size(per_tick * (std::uint64_t{drain_interval_ticks} + 1)), high_water(per_tick * drain_interval_ticks),
          event_drain(drained_by), write(0), read(0) {}

    EventRing(const EventRing&)            = delete;
    EventRing& operator=(const EventRing&) = delete;

    /// Executor: the events the drain has not consumed yet (design_engine_core.md §5.2)
    [[nodiscard]] std::uint64_t backlog() const noexcept {
        return write.load(std::memory_order_relaxed) - read.load(std::memory_order_acquire);
    }

    /// Executor: add an event. The gate keeps the ring from filling.
    void enqueue(const EngineEvent& event) noexcept {
        const std::uint64_t index = write.load(std::memory_order_relaxed); // our own word
        if (index - read.load(std::memory_order_acquire) == size) {
            abort_when_full();
        }
        slot[index % size] = event;
        write.store(index + 1, std::memory_order_release); // LINEARIZATION POINT
    }

    /**
     * Any thread: add an event raised outside a tick
     *
     * Its tick is the next tick to run, and the drain delivers it before that tick's ring
     * events (design_engine_core.md §5.2).
     */
    void raise_outside_tick(const EngineEvent& event) {
        std::list<EngineEvent> node{event}; // allocated before the lock
        std::lock_guard lk(side_m);
        side_list.splice(side_list.end(), node);
    }

    /**
     * The owner drain: append every event raised so far to `out`, in tick order, and free
     * their slots. Never blocks.
     *
     * Owner drain only. Under the sink the executor empties the ring itself, and a call here
     * is a programming error.
     */
    void drain(std::vector<EngineEvent>& out);

    /// Executor, under the sink: count every event raised so far, and keep none
    void drain_to_sink();

    /// Log a full ring as the programming error it is, and abort
    [[noreturn]] static void abort_when_full() noexcept;
};

} // namespace sim_estab::core::engine::event_ring
