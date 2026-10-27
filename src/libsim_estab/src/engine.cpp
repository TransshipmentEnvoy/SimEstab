module;

// compat
#include "compat.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module sim_estab;
import :engine.tick;
import :engine.lease;
import :engine.view;
import :engine.event_ring;
import :engine.gate;
import :engine.endpoint;

using namespace sim_estab::core::log;

namespace sim_estab::core::engine {

namespace {

constexpr const char *channel = "sim_estab.engine";

/// One live Engine per process (design_python_api.md §2). It stays set once a failed engine
/// is destroyed: the process is poisoned.
std::atomic<bool> engine_exists{false};

/// The host's source id (design_engine_core.md §2.4 step 3)
constexpr std::uint32_t host_source = 0;

/// The first tick a session runs: the freeze publishes tick 0 (design_engine_core.md §2.4
/// step 9)
constexpr Tick first_tick{1};

[[noreturn]] void abort_with(const char *what) noexcept {
    sim_estab_log(channel, severity_level::critical, "programming error: ", what);
    std::abort();
}

/// Log a failure, then throw it (design_patterns.md §6)
template <typename Error> [[noreturn]] void fail(const std::string& what) {
    sim_estab_log(channel, severity_level::error, what);
    throw Error(what);
}

const char *name_of(EngineState state) noexcept {
    switch (state) {
    case EngineState::Configuring:
        return "Configuring";
    case EngineState::Running:
        return "Running";
    case EngineState::Stopped:
        return "Stopped";
    case EngineState::Closing:
        return "Closing";
    case EngineState::Closed:
        return "Closed";
    case EngineState::LoadFailed:
        return "LoadFailed";
    case EngineState::Failed:
        return "Failed";
    }
    return "?";
}

const char *name_of(EventDrain drain) noexcept { return drain == EventDrain::Owner ? "Owner" : "Sink"; }

/// A call that found the operation lease closed (design_python_api.md §6)
[[noreturn]] void refuse_closed(const lease::Lease& control, const char *call) {
    if (control.state.load(std::memory_order_acquire) == lease::LeaseState::FAILED) {
        fail<engine_failed_error>(std::string(call) + ": the engine is Failed");
    }
    fail<engine_closed_error>(std::string(call) + ": the engine is closing or closed");
}

/// A seed for a session that configured none (design_engine_core.md §2.4 step 4)
std::uint64_t generate_seed() {
    std::random_device device;
    return (std::uint64_t{device()} << 32) | std::uint64_t{device()};
}

std::uint32_t paced_count(const std::vector<EngineViewSpec>& specs) noexcept {
    std::uint32_t count = 0;
    for (const EngineViewSpec& spec : specs) {
        count += spec.paced ? 1 : 0;
    }
    return count;
}

/// The config checks (design_patterns.md §6: validate arguments up front). The Python layer
/// validates first; here is where a C++ caller is told.
void validate(const EngineConfig& config) {
    if (config.tick_rate == 0) {
        fail<engine_error>("Engine: tick_rate must be at least 1, got 0");
    }
    if (config.drain_interval_ticks == 0) {
        fail<engine_error>("Engine: drain_interval_ticks (D) must be at least 1, got 0");
    }
    if (!(config.shutdown_deadline.count() > 0.0)) {
        fail<engine_error>("Engine: shutdown_deadline must be positive, got " +
                           std::to_string(config.shutdown_deadline.count()) + " s");
    }
    using Capacity = std::pair<const char *, std::uint32_t>;
    for (const auto& [name, capacity] :
         std::initializer_list<Capacity>{{"endpoint_capacity", config.endpoint_capacity},
                                         {"host_endpoint_capacity", config.host_endpoint_capacity},
                                         {"peer_endpoint_capacity", config.peer_endpoint_capacity}}) {
        if (capacity == 0 || capacity > limits::max_endpoint_capacity) {
            fail<engine_error>(std::string("Engine: ") + name + " must be 1 to " +
                               std::to_string(limits::max_endpoint_capacity) + ", got " + std::to_string(capacity));
        }
    }
}

/// Under the sink drain, the executor empties the event ring after every tick and when a step
/// ends (design_engine_core.md §5.2). Under the owner drain the owner thread empties it.
void drain_if_sink(gate::TickGate& tick_gate) {
    if (tick_gate.events.event_drain == event_ring::EventDrain::Sink) {
        tick_gate.events.drain_to_sink();
    }
}

/// Ends the step on every way out of run_step(), a throwing tick included
struct StepGuard {
    gate::TickGate& tick_gate;

    explicit StepGuard(gate::TickGate& g) : tick_gate(g) {}
    StepGuard(const StepGuard&)            = delete;
    StepGuard& operator=(const StepGuard&) = delete;
    ~StepGuard() {
        // Between steps the grant is revoked, which is a pause, and a pause discards the
        // deadline clock (design_engine_core.md §3.3). The next step starts a new one.
        tick_gate.wait_started.reset();
        // The step's last gate check may have raised an event that no later tick drains.
        drain_if_sink(tick_gate);
        tick_gate.end_step();
    }
};

/**
 * The session: what the freeze builds (design_engine_core.md §2.4), at fixed addresses for
 * the session's life, because threads hold atomics inside it
 */
struct Session {
    event_ring::EventRing events;
    gate::TickGate tick_gate;
    std::vector<EngineViewSpec> specs;         ///< the closed registry, by engine view index
    std::vector<std::uint32_t> participant_of; ///< per engine view: its participant; 0 unless paced
    std::vector<std::unique_ptr<view::EngineView>> engine_views;
    endpoint::Endpoint host;                       ///< source 0 (design_python_api.md §7.1)
    std::vector<endpoint::Endpoint *> drain_order; ///< ascending source id
    std::uint64_t seed;

    /// Steps 3, 3a and 4 of the freeze: the host endpoint, the engine views with their
    /// participants, the event ring sized for them, and the seed. Nothing here is observable.
    Session(const EngineConfig& config, std::vector<EngineViewSpec> registered, std::uint64_t session_seed)
        : events(event_ring::events_per_tick(0, 1, paced_count(registered), registered.size()),
                 config.drain_interval_ticks, *config.event_drain),
          tick_gate(paced_count(registered), first_tick, events), specs(std::move(registered)),
          host(tick_gate, host_source, config.host_endpoint_capacity), drain_order{&host}, seed(session_seed) {
        participant_of.reserve(specs.size());
        engine_views.reserve(specs.size());
        std::uint32_t participant = 0;
        for (const EngineViewSpec& spec : specs) {
            engine_views.push_back(std::make_unique<view::EngineView>(spec.max_payload_bytes));
            participant_of.push_back(spec.paced ? participant : 0);
            if (spec.paced) {
                gate::Participant& p = tick_gate.participants[participant];
                p.deadline_ms        = spec.deadline_ms;
                p.on_expiry          = spec.on_expiry;
                ++participant;
            }
        }
    }

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    /// Executor: publish `tick` into every engine view due by cadence. M1 publishes a tick
    /// and no bytes (design_engine_core.md §7 step 1).
    void publish(Tick tick) {
        for (std::size_t i = 0; i < engine_views.size(); ++i) {
            if (tick.value % specs[i].cadence != 0) {
                continue;
            }
            view::Block& block = engine_views[i]->writable_block(); // (P1)
            block.tick         = tick.value;
            block.size         = 0;
            engine_views[i]->publish(); // (P2)
        }
    }

    /// Executor: one tick: the drain, the tick itself and the publish (design_engine_core.md
    /// §3.3)
    void run_tick(Tick tick) {
        // M1: the one command does nothing (design_engine_core.md §7 step 1)
        endpoint::drain_commands(drain_order, tick, [](const endpoint::Command&, std::uint32_t, std::uint64_t) {});
        // execute(t): no system exists before M4
        publish(tick);
    }

    /// The engine view whose participant is `participant`
    [[nodiscard]] std::string_view participant_name(std::uint32_t participant) const noexcept {
        for (std::size_t i = 0; i < specs.size(); ++i) {
            if (specs[i].paced && participant_of[i] == participant) {
                return specs[i].name;
            }
        }
        return "?";
    }
};

} // namespace

StepResult run_step(gate::TickGate& tick_gate, std::uint64_t tick_count, std::function_ref<void(Tick)> run_tick,
                    std::function_ref<bool(Tick)> is_interrupted) {
    using gate::BlockerType;
    using Clock = gate::TickGate::Clock;

    const auto start = tick_gate.begin_step(tick_count);
    if (!start) {
        const StepEnd refusal =
            start.error() == gate::StepRefusal::StepInFlight ? StepEnd::StepInFlight : StepEnd::Overflow;
        return {refusal, Tick{tick_gate.first_unexecuted.load(std::memory_order_acquire) - 1}};
    }
    const StepGuard guard{tick_gate};

    Tick tick                    = *start;
    Clock::time_point next_check = Clock::now() + interrupt_check_slice;
    for (;;) {
        const gate::PassResult passed = tick_gate.pass(tick, next_check); // (G1) to (G4)
        switch (passed.blocker.type) {
        case BlockerType::NONE:
            run_tick(tick);
            drain_if_sink(tick_gate);
            tick_gate.finish_tick(tick); // (G5)
            tick = tick + 1;
            if (Clock::now() < next_check) {
                continue;
            }
            break;
        case BlockerType::STOP:
            return {StepEnd::Stopped, tick - 1};
        case BlockerType::EVENT_BACKLOG:
            return {StepEnd::EventBacklog, tick - 1};
        case BlockerType::HOST_PAUSE:
            return {StepEnd::GrantEnded, tick - 1};
        case BlockerType::PARTICIPANT:
            if (passed.expired) {
                return {StepEnd::Failed, tick - 1, passed.blocker.participant};
            }
            break;
        }

        // A slice has passed, at a tick boundary or while waiting at the gate.
        if (is_interrupted(tick - 1)) {
            return {StepEnd::Interrupted, tick - 1};
        }
        next_check = Clock::now() + interrupt_check_slice;
    }
}

// ---------------------------------------------------------------------------
// The engine view handle
// ---------------------------------------------------------------------------

EngineViewHandle::EngineViewHandle(std::shared_ptr<lease::Lease> control, view::EngineView& engine_view,
                                   gate::TickGate& tick_gate, const EngineViewSpec& spec,
                                   std::uint32_t participant) noexcept
    : control_(std::move(control)), engine_view_(&engine_view), tick_gate_(&tick_gate), name_(spec.name),
      cadence_(spec.cadence), paced_(spec.paced), participant_(participant) {}

bool EngineViewHandle::take() {
    if (!control_->try_enter()) {
        refuse_closed(*control_, "take()");
    }
    const lease::LeaseGuard guard{*control_};

    view::EngineView& engine_view = *engine_view_;
    // The block handed back is the one this reader consumed (design_engine_core.md §3.5).
    // The cadence hint and the predicate parameters arrive with projection (M5).
    engine_view.write_return(Tick{engine_view.held_block().tick}, 0, view::PredicateParams{});
    if (!engine_view.take()) { // (T1), (T2)
        return false;
    }
    if (paced_) {
        // The take is the declaration: ready through the next publish (design_engine_core.md §3.1)
        tick_gate_->declare_ready(participant_, Tick{engine_view.held_block().tick + cadence_});
    }
    return true;
}

const Block& EngineViewHandle::held() const {
    if (!control_->try_enter()) {
        refuse_closed(*control_, "held()");
    }
    const lease::LeaseGuard guard{*control_};
    return engine_view_->held_block();
}

std::string_view EngineViewHandle::name() const noexcept { return name_; }

std::uint32_t EngineViewHandle::cadence() const noexcept { return cadence_; }

bool EngineViewHandle::is_paced() const noexcept { return paced_; }

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

struct Engine::Impl {
    using Clock = lease::Lease::Clock;

    EngineConfig config;
    StepCallbacks step_callbacks;
    std::shared_ptr<lease::Lease> control; ///< the control block: the operation lease, shared with the handles
    std::atomic<EngineState> state{EngineState::Configuring};
    std::mutex control_m;       ///< the control mutex: state transitions, and the control calls before the freeze
    bool pending_pause = false; ///< guarded by control_m: a pause() made while Configuring
    bool pending_stop  = false; ///< guarded by control_m: a request_stop() made while Configuring
    std::vector<EngineViewSpec> specs; ///< the registry, until the freeze closes it
    std::unique_ptr<Session> session;  ///< from the freeze on; its address never changes

    Impl(const EngineConfig& engine_config, StepCallbacks callbacks)
        : config(engine_config), step_callbacks(std::move(callbacks)),
          control(std::make_shared<lease::Lease>(lease::LeaseState::OPEN)) {}

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() { cleanup(); }

    /// Close if the caller did not, then release the session, or disarm (design_patterns.md
    /// §4)
    void cleanup() noexcept {
        if (control->state.load(std::memory_order_acquire) == lease::LeaseState::OPEN) {
            sim_estab_log(channel, severity_level::warning, "engine destroyed without close(); closing it now");
            close();
        }
        const EngineState last = state.load(std::memory_order_acquire);
        if (last == EngineState::Failed) {
            // A thread may still reach the session, so nothing of it is freed, and the
            // singleton stays claimed: the process is poisoned (design_python_api.md §2).
            sim_estab_log(
                channel, severity_level::critical,
                "engine destroyed while Failed: its session (the gate, the host endpoint, ",
                session ? session->engine_views.size() : std::size_t{0},
                " engine views and the event ring) is leaked, and no Engine can be constructed again in this process");
            (void)session.release();
            return;
        }
        session.reset();
        sim_estab_log(channel, severity_level::info, "engine destroyed, from ", name_of(last));
        // The singleton was released by close(): a new Engine may live by now
    }

    /// Owner thread: close() (design_python_api.md §2 step 3)
    void close() noexcept {
        // Step 2 of the shutdown sequence, if the caller skipped it. Step 1, the mod hosts,
        // arrives with M7.
        if (state.load(std::memory_order_acquire) == EngineState::Running) {
            session->tick_gate.request_stop();
            set_state(EngineState::Stopped);
        }
        // Step 3. The compare-exchange is the linearization point: after it no call enters.
        lease::LeaseState expected = lease::LeaseState::OPEN;
        if (!control->state.compare_exchange_strong(expected, lease::LeaseState::CLOSING, std::memory_order_acq_rel)) {
            return; // already closing, closed or failed: nothing to do, and nothing to raise
        }
        set_state(EngineState::Closing);
        const Clock::duration budget = std::chrono::duration_cast<Clock::duration>(config.shutdown_deadline);
        if (!control->wait_for_holders(Clock::now() + budget)) { // the lease's read-modify-write
            control->state.store(lease::LeaseState::FAILED, std::memory_order_release);
            set_state(EngineState::Failed);
            sim_estab_log(channel, severity_level::critical,
                          "close(): ", control->active.load(std::memory_order_acquire),
                          " calls were still running after the shutdown deadline of ", config.shutdown_deadline.count(),
                          " s; the engine is Failed, and its session is kept rather than freed under a running call");
            return;
        }
        if (session) {
            // The host endpoint is revoked after its running submits finish
            // (design_python_api.md §7.1), under a budget of its own (design_limits.md §1.1).
            if (!session->host.revoke(Clock::now() + budget)) {
                control->state.store(lease::LeaseState::FAILED, std::memory_order_release);
                set_state(EngineState::Failed);
                sim_estab_log(
                    channel, severity_level::critical, "close(): the host endpoint's revocation timed out after ",
                    config.shutdown_deadline.count(),
                    " s; the engine is Failed, and its session is kept rather than freed under a running submit");
                return;
            }
            session.reset(); // the native release: the engine views, the endpoint, the gate, the ring
        }
        control->state.store(lease::LeaseState::CLOSED, std::memory_order_release);
        set_state(EngineState::Closed);
        sim_estab_log(channel, severity_level::info, "engine closed");
        // One live Engine per process (design_python_api.md §2): a closed one is not live,
        // so the next may be constructed while this object still exists. Only Failed keeps
        // the singleton, and poisons the process.
        engine_exists.store(false, std::memory_order_release);
    }

    /// Every call but close(): enter the operation lease, or throw for one that is not OPEN
    /// (design_python_api.md §6)
    lease::LeaseGuard enter(const char *call) {
        if (!control->try_enter()) {
            refuse_closed(*control, call);
        }
        return lease::LeaseGuard{*control};
    }

    /// Owner thread: move to `next`
    void set_state(EngineState next) {
        std::lock_guard lk(control_m);
        state.store(next, std::memory_order_release);
    }

    /// Owner thread: the session, for `call`, which needs one. The states without one throw.
    Session& session_for(const char *call, bool allow_stopped) {
        const EngineState now = state.load(std::memory_order_acquire);
        switch (now) {
        case EngineState::Running:
            return *session;
        case EngineState::Stopped:
            if (allow_stopped) {
                return *session;
            }
            fail<engine_state_error>(std::string(call) + ": the session has stopped, and no tick runs again");
        case EngineState::Configuring:
            fail<engine_state_error>(std::string(call) + ": the engine is Configuring; start_session() first");
        case EngineState::LoadFailed:
            fail<engine_state_error>(std::string(call) +
                                     ": the engine is LoadFailed; the freeze failed, so construct a new Engine");
        case EngineState::Failed:
            fail<engine_failed_error>(std::string(call) + ": the engine is Failed");
        case EngineState::Closing:
        case EngineState::Closed:
            fail<engine_closed_error>(std::string(call) + ": the engine is " + name_of(now));
        }
        abort_with("an EngineState outside its enumeration");
    }

    /// What a control call found: the state, and the session if there is one
    struct Control {
        EngineState state;
        Session *session;
    };

    /// Any thread, a control call. While Configuring, `pend` records the call for the
    /// freeze, under the control mutex, so the freeze cannot miss it.
    Control control_call(std::function_ref<void()> pend) {
        std::lock_guard lk(control_m);
        const EngineState now = state.load(std::memory_order_relaxed);
        if (now == EngineState::Configuring) {
            pend();
        }
        return {now, session.get()};
    }
};

namespace {

/// The session a control call acts on. Null while Configuring, where the call was recorded
/// for the freeze. The other states without a session to control refuse the call.
Session *session_to_control(EngineState state, Session *session, const char *call) {
    switch (state) {
    case EngineState::Running:
    case EngineState::Stopped:
        return session;
    case EngineState::Configuring:
        return nullptr;
    case EngineState::Failed:
        fail<engine_failed_error>(std::string(call) + ": the engine is Failed");
    case EngineState::LoadFailed:
        fail<engine_state_error>(std::string(call) + ": the engine is LoadFailed, and has no session to control");
    case EngineState::Closing:
    case EngineState::Closed:
        fail<engine_closed_error>(std::string(call) + ": the engine is " + name_of(state));
    }
    abort_with("an EngineState outside its enumeration");
}

} // namespace

Engine::Engine(const EngineConfig& config, StepCallbacks step_callbacks) {
    static_assert(sizeof(Impl) <= impl_size, "Engine::impl_size is too small for Engine::Impl");
    static_assert(alignof(Impl) <= impl_align, "Engine::impl_align is too small for Engine::Impl");

    validate(config);
    bool expected = false;
    if (!engine_exists.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        fail<engine_exists_error>("Engine: an Engine already exists in this process; one lives at a time, and a "
                                  "failed one poisons the process (design_python_api.md §2)");
    }
    try {
        impl_ = ::new (static_cast<void *>(impl_buffer_)) Impl(config, std::move(step_callbacks));
        sim_estab_log(channel, severity_level::info, "engine created: Configuring, tick rate ", config.tick_rate, ", ",
                      impl_->step_callbacks.size(), " step callbacks");
    } catch (...) {
        if (impl_ != nullptr) {
            impl_->~Impl(); // cleanup() closes, and close() releases the singleton
            impl_ = nullptr;
        } else {
            engine_exists.store(false, std::memory_order_release);
        }
        throw;
    }
}

Engine::~Engine() noexcept {
    if (impl_ != nullptr) {
        impl_->~Impl();
        impl_ = nullptr;
    }
}

void Engine::check_impl() const noexcept {
    if (impl_ == nullptr) {
        abort_with("Engine used before construction or after destruction");
    }
}

EngineState Engine::state() const noexcept {
    if (impl_ == nullptr) {
        return EngineState::Closed; // a query degrades (design_patterns.md §5a)
    }
    return impl_->state.load(std::memory_order_acquire);
}

EngineConfig Engine::config() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->config;
}

std::uint64_t Engine::seed() const {
    check_impl();
    const lease::LeaseGuard guard = impl_->enter("seed()");
    return impl_->session_for("seed()", true).seed;
}

void Engine::register_engine_view(const EngineViewSpec& spec) {
    check_impl();
    Impl& impl                    = *impl_;
    const lease::LeaseGuard guard = impl.enter("register_engine_view()");
    const EngineState now         = impl.state.load(std::memory_order_acquire);
    if (now != EngineState::Configuring) {
        fail<engine_state_error>(std::string("register_engine_view(): the engine is ") + name_of(now) +
                                 "; an engine view is registered while Configuring, before start_session()");
    }
    if (spec.name.empty()) {
        fail<engine_error>("register_engine_view(): an engine view needs a name");
    }
    for (const EngineViewSpec& other : impl.specs) {
        if (other.name == spec.name) {
            fail<engine_error>("register_engine_view(): an engine view named '" + spec.name +
                               "' is already registered");
        }
    }
    if (spec.cadence == 0) {
        fail<engine_error>("register_engine_view(): cadence must be at least 1, got 0 for '" + spec.name + "'");
    }
    if (spec.predicate != RowPredicateType::All) {
        fail<engine_error>("register_engine_view(): a row predicate type that does not exist yet for '" + spec.name +
                           "'");
    }
    if (impl.specs.size() >= limits::max_engine_views) {
        fail<engine_error>("register_engine_view(): '" + spec.name + "' would be engine view number " +
                           std::to_string(impl.specs.size() + 1) + ", past the " +
                           std::to_string(limits::max_engine_views) + " a session may register");
    }
    impl.specs.push_back(spec);
    sim_estab_log(channel, severity_level::debug, "engine view '", spec.name, "' registered: cadence ", spec.cadence,
                  spec.paced ? ", paced" : ", unpaced", ", ", spec.max_payload_bytes, " payload bytes");
}

void Engine::start_session() {
    check_impl();
    Impl& impl                    = *impl_;
    const lease::LeaseGuard guard = impl.enter("start_session()");
    const EngineState now         = impl.state.load(std::memory_order_acquire);
    if (now != EngineState::Configuring) {
        fail<engine_state_error>(std::string("start_session(): the engine is ") + name_of(now) +
                                 "; the freeze runs once, from Configuring");
    }
    if (!impl.config.event_drain) {
        fail<engine_error>("start_session(): EngineConfig.event_drain is not set; every session declares who "
                           "empties its event ring, Owner or Sink (design_python_api.md §3)");
    }

    // The freeze (design_engine_core.md §2.4). Steps 1, 2, 5, 6 and 7 have nothing to do
    // until the world (M3), identity and replay (M2) and mods (M7) exist. Steps 3, 3a and 4
    // build the session aside, where a failure leaves nothing observable.
    const std::uint64_t seed = impl.config.seed ? *impl.config.seed : generate_seed(); // step 4
    std::unique_ptr<Session> session;
    try {
        session = std::make_unique<Session>(impl.config, impl.specs, seed); // steps 3, 3a
    } catch (...) {
        impl.set_state(EngineState::LoadFailed);
        sim_estab_log(channel, severity_level::error,
                      "start_session(): the freeze failed while building the session; the engine is LoadFailed");
        throw;
    }
    // Step 8: the per-session log sink opens here once it exists (design_logging.md §4).
    // Step 9: tick 0 into every engine view, session.started, and the host endpoint opens.
    // The session is still private, so nothing of this is seen before it is installed.
    session->publish(Tick{0});
    session->events.raise_outside_tick({first_tick.value, EngineEventType::SessionStarted});
    session->host.open();

    bool pause_pending = false;
    bool stop_pending  = false;
    {
        std::lock_guard lk(impl.control_m);
        impl.session = std::move(session);
        impl.specs.clear();
        pause_pending = std::exchange(impl.pending_pause, false);
        stop_pending  = std::exchange(impl.pending_stop, false);
        impl.state.store(EngineState::Running, std::memory_order_release);
    }
    // The control calls made while Configuring, outside the control mutex: a control call
    // that lands now sees Running and acts on the gate itself.
    Session& installed = *impl.session;
    if (pause_pending) {
        installed.tick_gate.pause();
    }
    if (stop_pending) {
        installed.tick_gate.request_stop();
    }
    sim_estab_log(channel, severity_level::info, "session started: Running, seed ", installed.seed, ", ",
                  installed.engine_views.size(), " engine views, ", installed.tick_gate.participant_count,
                  " participants, event drain ", name_of(installed.events.event_drain));
}

StepResult Engine::step(std::uint64_t tick_count) {
    check_impl();
    Impl& impl                    = *impl_;
    const lease::LeaseGuard guard = impl.enter("step()");
    Session& session              = impl.session_for("step()", false);

    std::string interrupted_by;
    const auto run_tick       = [&](Tick tick) { session.run_tick(tick); };
    const auto is_interrupted = [&](Tick tick_reached) {
        for (const auto& [name, callback] : impl.step_callbacks) {
            if (callback(tick_reached)) {
                interrupted_by = name;
                return true;
            }
        }
        return false;
    };
    StepResult result = run_step(session.tick_gate, tick_count, run_tick, is_interrupted);

    switch (result.end) {
    case StepEnd::GrantEnded:
        break;
    case StepEnd::Stopped:
        impl.set_state(EngineState::Stopped);
        sim_estab_log(channel, severity_level::info, "session stopped at tick ", result.tick_reached.value,
                      ": no tick runs again");
        break;
    case StepEnd::Interrupted:
        result.interrupted_by = std::move(interrupted_by);
        break;
    case StepEnd::EventBacklog: {
        const std::string what = "step(): the event backlog reached its mark at tick " +
                                 std::to_string(result.tick_reached.value) + "; drain_events(), then step again";
        sim_estab_log(channel, severity_level::error, what);
        throw event_backlog_error(what, result.tick_reached);
    }
    case StepEnd::Failed:
        // Every later call fails, the handles' included: the lease says so too.
        impl.control->state.store(lease::LeaseState::FAILED, std::memory_order_release);
        impl.set_state(EngineState::Failed);
        fail<engine_failed_error>("step(): participant " + std::to_string(result.participant) + " (engine view '" +
                                  std::string(session.participant_name(result.participant)) +
                                  "') was not ready within its deadline under OnExpiry::Fail; the engine is Failed");
    case StepEnd::StepInFlight:
        fail<engine_state_error>("step(): a step is in flight");
    case StepEnd::Overflow:
        fail<engine_error>("step(): " + std::to_string(tick_count) + " ticks from tick " +
                           std::to_string(result.tick_reached.value + 1) + " do not fit the tick counter");
    }
    return result;
}

std::expected<CommandHandle, Rejection> Engine::submit(const Command& command) {
    check_impl();
    const lease::LeaseGuard guard = impl_->enter("submit()");
    return impl_->session_for("submit()", true).host.submit(command);
}

CommandOutcome Engine::outcome(const CommandHandle& handle) const {
    check_impl();
    const lease::LeaseGuard guard = impl_->enter("outcome()");
    Session& session              = impl_->session_for("outcome()", true);
    if (handle.source != host_source) {
        fail<engine_error>("outcome(): the handle names source " + std::to_string(handle.source) +
                           ", and this session has only the host endpoint, source 0");
    }
    // The producer's own word: the owner thread is the host endpoint's producer.
    if (handle.position >= session.host.ring.write.load(std::memory_order_relaxed)) {
        fail<engine_error>("outcome(): the handle names a command the host endpoint has not issued");
    }
    return session.host.outcome(handle);
}

EngineViewHandle Engine::view(std::string_view name) {
    check_impl();
    const lease::LeaseGuard guard = impl_->enter("view()");
    Session& session              = impl_->session_for("view()", true);
    for (std::size_t i = 0; i < session.specs.size(); ++i) {
        if (session.specs[i].name == name) {
            return EngineViewHandle{impl_->control, *session.engine_views[i], session.tick_gate, session.specs[i],
                                    session.participant_of[i]};
        }
    }
    fail<engine_error>("view(): no engine view named '" + std::string(name) + "' was registered");
}

void Engine::drain_events(std::vector<EngineEvent>& out) {
    check_impl();
    const lease::LeaseGuard guard = impl_->enter("drain_events()");
    Session& session              = impl_->session_for("drain_events()", true);
    if (session.events.event_drain != EventDrain::Owner) {
        fail<engine_state_error>("drain_events(): the session declared a sink drain, and the engine empties the "
                                 "ring itself; sink_digest() reports what it consumed");
    }
    session.events.drain(out);
}

SinkDigest Engine::sink_digest() const {
    check_impl();
    const lease::LeaseGuard guard = impl_->enter("sink_digest()");
    Session& session              = impl_->session_for("sink_digest()", true);
    if (session.events.event_drain != EventDrain::Sink) {
        fail<engine_state_error>("sink_digest(): the session declared an owner drain, and has no sink; "
                                 "drain_events() empties the ring");
    }
    return {session.events.sink_count};
}

Tick Engine::snapshot() {
    check_impl();
    const lease::LeaseGuard guard = impl_->enter("snapshot()");
    Session& session              = impl_->session_for("snapshot()", true);
    bool in_flight                = false;
    {
        std::lock_guard lk(session.tick_gate.gate.m);
        in_flight = session.tick_gate.host.step_in_flight;
    }
    if (in_flight) {
        fail<engine_state_error>("snapshot(): a step is in flight; the owner thread reads the world only while no "
                                 "tick can run");
    }
    return Tick{session.tick_gate.first_unexecuted.load(std::memory_order_acquire) - 1};
}

void Engine::pause() {
    check_impl();
    Impl& impl                    = *impl_;
    const lease::LeaseGuard guard = impl.enter("pause()");
    const Impl::Control control   = impl.control_call([&] { impl.pending_pause = true; });
    if (Session *session = session_to_control(control.state, control.session, "pause()")) {
        session->tick_gate.pause();
    }
}

void Engine::resume() {
    check_impl();
    Impl& impl                    = *impl_;
    const lease::LeaseGuard guard = impl.enter("resume()");
    const Impl::Control control   = impl.control_call([&] { impl.pending_pause = false; });
    if (Session *session = session_to_control(control.state, control.session, "resume()")) {
        if (!session->tick_gate.resume()) {
            fail<engine_state_error>("resume(): a step is in flight; it holds the run grant until it returns");
        }
    }
}

void Engine::request_stop() {
    check_impl();
    Impl& impl                    = *impl_;
    const lease::LeaseGuard guard = impl.enter("request_stop()");
    const Impl::Control control   = impl.control_call([&] { impl.pending_stop = true; });
    if (Session *session = session_to_control(control.state, control.session, "request_stop()")) {
        session->tick_gate.request_stop();
    }
}

bool Engine::stop_requested() const noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    Impl& impl = *impl_;
    if (!impl.control->try_enter()) {
        return false; // a query degrades: a closed engine has nothing left to stop
    }
    const lease::LeaseGuard guard{*impl.control};
    std::lock_guard lk(impl.control_m);
    if (impl.session) {
        return impl.session->tick_gate.host.stop_requested.load(std::memory_order_acquire) != 0;
    }
    return impl.pending_stop;
}

void Engine::stop_sim_async() {
    check_impl();
    Impl& impl                    = *impl_;
    const lease::LeaseGuard guard = impl.enter("stop_sim_async()");
    const EngineState now         = impl.state.load(std::memory_order_acquire);
    switch (now) {
    case EngineState::Running:
        impl.session->tick_gate.request_stop();
        impl.set_state(EngineState::Stopped);
        sim_estab_log(channel, severity_level::info, "session stopped by stop_sim_async(): no tick runs again");
        return;
    case EngineState::Configuring:
    case EngineState::Stopped:
    case EngineState::LoadFailed:
        return; // nothing runs ticks
    case EngineState::Failed:
        fail<engine_failed_error>("stop_sim_async(): the engine is Failed");
    case EngineState::Closing:
    case EngineState::Closed:
        fail<engine_closed_error>(std::string("stop_sim_async(): the engine is ") + name_of(now));
    }
}

void Engine::close() noexcept {
    if (impl_ == nullptr) {
        return; // destroyed already: nothing left to close
    }
    impl_->close();
}

event_ring::EventRing& session_events(Engine& engine) {
    engine.check_impl();
    return engine.impl_->session_for("session_events()", true).events;
}

lease::Lease& engine_control(Engine& engine) {
    engine.check_impl();
    return *engine.impl_->control;
}

} // namespace sim_estab::core::engine
