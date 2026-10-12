/**
 * @file sim_estab--engine.cppm
 * @brief Module partition for the engine
 *
 * The engine drives the mechanisms of the boundary. This partition holds the Engine: the
 * session lifecycle, Configuring, the freeze, Running and Stopped (design_engine_core.md
 * §2.4, design_python_api.md §2); the headless executor step(n), which runs ticks on the
 * calling thread (design_python_api.md §4.3); and the calls that reach the mechanisms: the
 * host endpoint, the engine view handles, the event drains and the snapshot. It also holds
 * the tick loop that step(n) runs: the loop that passes the gate, runs one tick and repeats
 * (design_engine_core.md §3.3). The gate itself is in sim_estab:engine.gate.
 *
 * M1 runs headless and carries no simulation content (design_engine_core.md §7 step 1). A
 * tick drains the host endpoint, executes nothing, and publishes its tick and zero bytes
 * into every engine view due by cadence. snapshot() returns the tick alone.
 *
 * Threads. Construction, the freeze, step(), submit(), outcome(), view(), drain_events(),
 * sink_digest(), snapshot() and stop_sim_async() belong to the owner thread, the thread that
 * constructed the Engine; the Python layer checks it (design_python_api.md §6). The control
 * calls pause(), resume(), request_stop() and stop_requested() may come from any thread. An
 * EngineViewHandle belongs to its engine view's one reader thread. One short mutex, the
 * control mutex, guards the state transitions and the control calls made before the
 * freeze; it is never held together with another engine mutex (design_engine_core.md
 * §1.1).
 *
 * Every call but close(), state() and config() holds the operation lease while it runs
 * (design_python_api.md §6). close() closes it, waits for the running calls under the
 * shutdown deadline, and only then releases the session. The lease is the control block
 * the Engine shares with its handles: a handle used after close(), or after the engine is
 * gone, throws instead of touching freed memory.
 *
 * It exports the Engine, its config, errors and handle, the tick type as engine::Tick, and
 * the value types of the mechanisms under the engine's name. The tick loop is internal, and
 * its tests are units of the module (design_patterns.md §10).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <flat_map>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Module declaration
export module sim_estab:engine;

import :error;
import :limits;
import :engine.tick;
import :engine.lease;
import :engine.view;
import :engine.event_ring;
import :engine.gate;
import :engine.endpoint;

/**
 * @namespace sim_estab::core::engine
 * @brief The engine: the session, the executor and the tick loop
 */
namespace sim_estab::core::engine {

/// The tick, under the engine's name. Its home is sim_estab:engine.tick.
export using tick::Tick;

/// The value types of the engine's mechanisms, under the engine's name. Each one's home is
/// its mechanism's partition.
export using view::Block;
export using event_ring::EngineEvent;
export using event_ring::EngineEventType;
export using event_ring::EventDrain;
export using gate::OnExpiry;
export using endpoint::Command;
export using endpoint::CommandHandle;
export using endpoint::CommandOutcome;
export using endpoint::CommandOutcomeType;
export using endpoint::Rejection;
export using endpoint::provisional_command_payload_bytes;

// ---------------------------------------------------------------------------
// Errors (design_patterns.md §6, design_python_api.md §8)
// ---------------------------------------------------------------------------

/// The engine's error: a bad argument, such as a duplicate engine view name. The classes
/// below derive from it, one per case a caller tells apart.
export class engine_error : public error::sim_estab_error {
public:
    explicit engine_error(const std::string& what) : error::sim_estab_error(what) {}
};

/// A call the engine's state does not allow: a session call while Configuring, a step
/// while one is in flight, a step once the session has stopped
export class engine_state_error : public engine_error {
public:
    explicit engine_state_error(const std::string& what) : engine_error(what) {}
};

/// A call that began once close() had started, or on a closed engine or handle
/// (design_python_api.md §2). Terminal for that engine.
export class engine_closed_error : public engine_error {
public:
    explicit engine_closed_error(const std::string& what) : engine_error(what) {}
};

/// A step under an owner drain reached the event backlog mark. The caller drains and steps
/// again (design_engine_core.md §5.2).
export class event_backlog_error : public engine_error {
public:
    event_backlog_error(const std::string& what, Tick reached) : engine_error(what), tick_reached_(reached) {}

    /// The last tick that has run
    [[nodiscard]] Tick tick_reached() const noexcept { return tick_reached_; }

private:
    Tick tick_reached_;
};

/// The engine is in the terminal Failed state, and so is every later call
export class engine_failed_error : public engine_error {
public:
    explicit engine_failed_error(const std::string& what) : engine_error(what) {}
};

/// A second Engine in the process. One lives at a time, and a failed one poisons the process
/// (design_python_api.md §2).
export class engine_exists_error : public engine_error {
public:
    explicit engine_exists_error(const std::string& what) : engine_error(what) {}
};

// ---------------------------------------------------------------------------
// The session's declarations
// ---------------------------------------------------------------------------

/// The engine state (design_python_api.md §2). Closing and Closed arrive with close().
export enum class EngineState : std::uint32_t {
    Configuring, ///< resources acquired, no world: register, then start_session()
    Running,     ///< the session exists; ticks run in step()
    Stopped,     ///< no tick runs again; reads still work
    Closing,     ///< close() is waiting for running calls
    Closed,      ///< close() has released everything
    LoadFailed,  ///< terminal: the freeze failed, nothing leaked, construct a new Engine
    Failed,      ///< terminal: live execution could not be stopped safely; the process is poisoned
};

/// The row predicate type an engine view declares (design_engine_core.md §3.4). AABB,
/// Sphere, Frustum and Tag arrive with projection (M5).
export enum class RowPredicateType : std::uint32_t {
    All, ///< every live row
};

/// The engine's options: the C++ mirror of the Python EngineConfig, converted once at the
/// boundary (design_python_api.md §3). The defaults are the policy defaults of
/// sim_estab:limits.
export struct EngineConfig {
    std::uint32_t tick_rate = limits::default_tick_rate; ///< part of session identity; not a speed
    std::optional<std::uint64_t> seed;                   ///< none: generated at the freeze, then kept
    /// Who empties the event ring. No default: start_session() refuses a session without one.
    std::optional<EventDrain> event_drain;
    std::uint32_t drain_interval_ticks   = limits::default_drain_interval_ticks;   ///< D
    std::uint32_t endpoint_capacity      = limits::default_endpoint_capacity;      ///< a mod endpoint's, by default
    std::uint32_t host_endpoint_capacity = limits::default_host_endpoint_capacity; ///< source 0's
    std::uint32_t peer_endpoint_capacity = limits::default_peer_endpoint_capacity; ///< a peer endpoint's
    /// Seconds each blocking shutdown stage may wait (design_limits.md §1.1): the mirror of
    /// the Python HostPolicy.shutdown_deadline
    std::chrono::duration<double> shutdown_deadline{limits::default_shutdown_deadline_seconds};
};

/// What an engine view declares at registration (design_engine_core.md §3.1). The freeze
/// closes it.
export struct EngineViewSpec {
    std::string name;                       ///< how engine.view() finds it; unique in a session
    std::uint32_t max_payload_bytes = 0;    ///< the M1 stand-in for the projection spec: bytes per block
    bool identity                   = true; ///< whether the id column is projected (M5)
    RowPredicateType predicate      = RowPredicateType::All;
    std::uint32_t cadence           = 1;              ///< published every `cadence` ticks; at least 1
    bool paced                      = false;          ///< its reader is a participant: each take declares ready
    std::uint32_t deadline_ms       = 0;              ///< paced: how long the gate waits for a take
    OnExpiry on_expiry              = OnExpiry::Fail; ///< paced: what a passed deadline does
};

/// What the sink drain has consumed (design_engine_core.md §5.2). The hash arrives with the
/// event payload (open_question.md Q85).
export struct SinkDigest {
    std::uint64_t count = 0;
};

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

/// A step calls its step callbacks at least once per slice: at a tick boundary, and between
/// slices while it waits at the gate (design_python_api.md §4.3)
inline constexpr std::chrono::milliseconds interrupt_check_slice{100};

/**
 * A step callback: a hook the host process registers at construction, by name
 * (design_python_api.md §4.3)
 *
 * step() calls each one at most once per interrupt_check_slice, at a tick boundary or
 * between parks at the gate, in key order, with the last tick that has run. The gate is not
 * held and no tick is in progress. A callback that returns true ends the step at that
 * boundary: no tick is cut short, the rest of the grant is revoked, and the later callbacks
 * are not called. One that only observes returns false. An exception it throws ends the
 * step the same way and propagates out of step(). It is not a per-tick callback: the Python
 * binding registers the signal check and nothing else (design_python_api.md §5).
 */
export using StepCallback = std::function<bool(Tick)>;

/// The step callbacks, by name. Fixed for the engine's life.
export using StepCallbacks = std::flat_map<std::string, StepCallback>;

/// What ended a step, or refused it
export enum class StepEnd : std::uint32_t {
    GrantEnded,   ///< the granted ticks have run, or a pause ended the grant early
    Stopped,      ///< a stop was requested
    EventBacklog, ///< the event backlog reached its mark; the caller drains and steps again
    Interrupted,  ///< a step callback asked the step to end
    Failed,       ///< a participant's deadline passed under OnExpiry::Fail
    StepInFlight, ///< refused: another step is in flight
    Overflow,     ///< refused: the tick count does not fit
};

/// How a step ended
export struct StepResult {
    StepEnd end = StepEnd::GrantEnded;
    Tick tick_reached;             ///< the last tick that has run
    std::uint32_t participant = 0; ///< Failed: the participant whose deadline passed
    std::string interrupted_by;    ///< Interrupted: the step callback that ended the step
};

/**
 * Run a step on the calling thread, which is the executor until the call returns
 * (design_engine_core.md §3.3)
 *
 * The loop passes the gate, runs one tick and repeats. It ends when the grant of
 * `tick_count` ticks is used up or a pause ends it early, at a stop, and at the event
 * backlog mark. It waits only for a participant that is not ready, up to its deadline.
 * Whatever ends the step, no tick is cut short, and what is left of the grant is revoked.
 * Under the sink drain it empties the event ring after every tick and when the step ends
 * (design_engine_core.md §5.2).
 *
 * The Engine wraps it; it is not exported.
 *
 * @param tick_gate the session's gate
 * @param tick_count how many ticks to run
 * @param run_tick runs one tick: the command drain, the tick itself and the publish
 * @param is_interrupted true if the step should end at this boundary. Called at most once
 *        per interrupt_check_slice, with the last tick that has run.
 */
[[nodiscard]] StepResult run_step(gate::TickGate& tick_gate, std::uint64_t tick_count,
                                  std::function_ref<void(Tick)> run_tick, std::function_ref<bool(Tick)> is_interrupted);

// ---------------------------------------------------------------------------
// The engine view handle
// ---------------------------------------------------------------------------

export class Engine;

/**
 * The reader's handle on an engine view (design_engine_core.md §3.1), from Engine::view()
 *
 * It belongs to the engine view's one reader thread, whichever thread that is. Two threads
 * taking from one engine view is a usage error (design_python_api.md §6). It shares the
 * engine's control block, so after close(), or once the engine is gone, its calls throw
 * engine_closed_error instead of touching freed memory (design_python_api.md §2).
 */
export class EngineViewHandle {
public:
    /**
     * Reader: take the newest published block, handing back the one held
     *
     * It writes the return header first: the tick of the block it hands back is the last
     * tick it consumed (design_engine_core.md §3.5). On a paced engine view a successful
     * take is the reader's declaration: ready through the taken tick plus the cadence
     * (design_engine_core.md §3.1).
     *
     * @return true if a newer block was taken. false if nothing new was published: the
     *         reader keeps the block it holds, and declares nothing.
     * @throws engine_closed_error once the engine is closing or closed; engine_failed_error
     *         once it has failed
     */
    [[nodiscard]] bool take();

    /// Reader: the block it holds. It shows a snapshot once the first take() has succeeded.
    /// The reference is valid until the next take(), or until the engine closes.
    /// @throws engine_closed_error once the engine is closing or closed; engine_failed_error
    ///         once it has failed
    [[nodiscard]] const Block& held() const;

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::uint32_t cadence() const noexcept;
    [[nodiscard]] bool is_paced() const noexcept;

private:
    friend class Engine;
    EngineViewHandle(std::shared_ptr<lease::Lease> control, view::EngineView& engine_view, gate::TickGate& tick_gate,
                     const EngineViewSpec& spec, std::uint32_t participant) noexcept;

    std::shared_ptr<lease::Lease> control_; ///< the control block it shares with the engine
    view::EngineView *engine_view_;
    gate::TickGate *tick_gate_;
    std::string name_;          ///< its own copy: readable after the session is gone
    std::uint32_t cadence_;     ///< its own copy
    bool paced_;                ///< its own copy
    std::uint32_t participant_; ///< paced: the reader's participant at the gate
};

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

/**
 * The engine (design_python_api.md §2): one per process, a RAII context
 * (design_patterns.md §4)
 *
 * Construction acquires and leaves the engine Configuring. Engine views are registered,
 * then start_session() runs the freeze (design_engine_core.md §2.4) and the session is
 * Running: step() runs ticks, the host endpoint admits commands, engine views publish. A
 * stop makes it Stopped; reads still work. close() waits for the running calls, releases
 * the session and leaves the engine Closed; the destructor closes if the caller did not.
 * In the Failed state nothing is released: the session is leaked and the process stays
 * poisoned.
 *
 * Every call but close(), state() and config() throws engine_closed_error once close() has
 * begun, and engine_failed_error once the engine is Failed.
 */
export class Engine {
public:
    /**
     * Acquire, and leave the engine Configuring
     *
     * @param config validated here: a tick rate and a drain interval of at least 1, and
     *        capacities in 1..max_endpoint_capacity. Throws engine_error otherwise.
     * @param step_callbacks the step callbacks, fixed for the engine's life
     * @throws engine_exists_error if an Engine already exists in this process
     */
    explicit Engine(const EngineConfig& config, StepCallbacks step_callbacks = {});
    ~Engine() noexcept;

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&)                 = delete;
    Engine& operator=(Engine&&)      = delete;

    /// Any thread: the engine state
    [[nodiscard]] EngineState state() const noexcept;

    /// The config the engine was constructed with
    [[nodiscard]] EngineConfig config() const noexcept;

    /// Running or Stopped: the seed the freeze resolved, from the config or generated
    [[nodiscard]] std::uint64_t seed() const;

    /**
     * Configuring: declare an engine view (design_engine_core.md §3.1)
     *
     * @throws engine_state_error once the session has started
     * @throws engine_error for an empty or duplicate name, a cadence of 0, or the 65th
     *         engine view (limits::max_engine_views)
     */
    void register_engine_view(const EngineViewSpec& spec);

    /**
     * Configuring: the freeze (design_engine_core.md §2.4)
     *
     * It builds the session aside, then installs it: tick 0 is published into every engine
     * view, session.started is raised, the host endpoint opens, and the engine is Running.
     * Control calls made while Configuring apply now. A failure while building leaves the
     * engine LoadFailed and nothing observable.
     *
     * @throws engine_state_error unless Configuring
     * @throws engine_error if the config declares no event drain
     */
    void start_session();

    /**
     * Running: run `tick_count` ticks on the calling thread (design_python_api.md §4.3)
     *
     * @return the tick reached, and what ended the step: GrantEnded, Stopped or
     *         Interrupted (with the callback's name). A stop leaves the engine Stopped.
     * @throws engine_state_error unless Running, or while a step is in flight
     * @throws engine_error if `tick_count` does not fit the tick counter
     * @throws event_backlog_error at the backlog mark under an owner drain; the caller
     *         drains and steps again
     * @throws engine_failed_error when a participant's deadline passes under
     *         OnExpiry::Fail; the engine is Failed
     */
    [[nodiscard]] StepResult step(std::uint64_t tick_count);

    /// Running or Stopped: submit a command on the host endpoint. Never waits: a full ring
    /// is QueueFull (design_engine_core.md §5.1).
    [[nodiscard]] std::expected<CommandHandle, Rejection> submit(const Command& command);

    /// Running or Stopped: a host command's outcome. Never waits.
    /// @throws engine_error for a handle the host endpoint did not issue
    [[nodiscard]] CommandOutcome outcome(const CommandHandle& handle) const;

    /// Running or Stopped: the handle of the engine view registered as `name`
    /// @throws engine_error if no engine view has that name
    [[nodiscard]] EngineViewHandle view(std::string_view name);

    /// Running or Stopped, owner drain: append every event raised so far to `out`, in
    /// tick order. Never blocks.
    /// @throws engine_state_error under a sink drain
    void drain_events(std::vector<EngineEvent>& out);

    /// Running or Stopped, sink drain: what the sink has consumed
    /// @throws engine_state_error under an owner drain
    [[nodiscard]] SinkDigest sink_digest() const;

    /// Running or Stopped, while no step is in flight: the last tick that has run. The
    /// M1 snapshot is the tick alone (design_engine_core.md §7 step 1).
    /// @throws engine_state_error while a step is in flight
    [[nodiscard]] Tick snapshot();

    /// Any thread: no tick runs after the one in progress, until a resume or a step.
    /// Before the freeze it applies at the freeze.
    void pause();

    /// Any thread: lift the ceiling on the run grant. Before the freeze it cancels a pause.
    /// @throws engine_state_error while a step is in flight
    void resume();

    /// Any thread: no tick runs again in this session. Sticky. Before the freeze it
    /// applies at the freeze.
    void request_stop();

    /// Any thread: whether a stop was requested
    [[nodiscard]] bool stop_requested() const noexcept;

    /// Running: end ticking for the session; the engine is Stopped (design_python_api.md
    /// §2). With no sim thread there is nothing to join. Nothing to do in any other state
    /// but Failed, which throws.
    void stop_sim_async();

    /**
     * Owner thread: shut down (design_python_api.md §2 step 3). Never throws, and a second
     * call does nothing.
     *
     * It ends ticking if the caller has not (stop_sim_async()), closes the operation lease
     * (Closing), waits for the running calls under the shutdown deadline, revokes the host
     * endpoint under a budget of its own, releases the session and is Closed. A deadline
     * that passes leaves the engine Failed, its session kept rather than freed under a
     * running call. In Failed it does nothing. The destructor calls it if the caller did
     * not.
     */
    void close() noexcept;

private:
    struct Impl;
    static constexpr std::size_t impl_size  = 512; // headroom, not exact
    static constexpr std::size_t impl_align = alignof(void *);

    alignas(impl_align) unsigned char impl_buffer_[impl_size];
    Impl *impl_ = nullptr; // cached; nullptr = not constructed

    void check_impl() const noexcept; // aborts on null, even in release

    friend event_ring::EventRing& session_events(Engine& engine);
    friend lease::Lease& engine_control(Engine& engine);
};

/// Units of the module only (the tests): the session's event ring, to raise the events an
/// M1 session never raises by itself. Not exported.
[[nodiscard]] event_ring::EventRing& session_events(Engine& engine);

/// Units of the module only (the tests): the control block, to hold the operation lease
/// as a call that never returns would. Not exported.
[[nodiscard]] lease::Lease& engine_control(Engine& engine);

} // namespace sim_estab::core::engine
