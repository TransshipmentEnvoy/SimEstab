// nanobind first: it includes Python.h, which must precede the standard headers
// because it defines _POSIX_C_SOURCE and _XOPEN_SOURCE
#include <nanobind/nanobind.h>
#include <nanobind/stl/chrono.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

// Standard library headers
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sim_estab/compat.h>
import sim_estab;

/**
 * The nanobind extension sim_estab._if: a 1:1 mirror of the bound C++ API
 * (design_patterns.md §8). A partition of sim_estab is a submodule of the same name, a
 * bound name keeps its C++ spelling, and everything Pythonic lives above it, in the pure
 * Python package (design_python_api.md §1).
 */

namespace nb = nanobind;

namespace {

namespace error     = sim_estab::core::error;
namespace estab_log = sim_estab::core::log; // not `log`: that name is the C function
namespace limits    = sim_estab::core::limits;
namespace engine    = sim_estab::core::engine;

/**
 * The signal check: the one step callback the binding registers
 * (design_python_api.md §4.3, §5 rule 3)
 *
 * step() releases the GIL and calls it at most once per interrupt_check_slice, at a tick
 * boundary or between parks at the gate. A pending signal runs its Python handler here;
 * a KeyboardInterrupt it raises ends the step at that boundary and propagates out of
 * step(). No tick is cut short.
 */
bool signal_check(engine::Tick) {
    const nb::gil_scoped_acquire gil;
    if (PyErr_CheckSignals() != 0) {
        throw nb::python_error();
    }
    return false;
}

/// The root of the exception family, in _if.error (design_patterns.md §8). It is bound
/// before any derived exception: nanobind tries the newest translator first, so a derived
/// exception must register after its base to be raised as itself.
nb::exception<error::sim_estab_error>& bind_error(nb::module_& m) {
    nb::module_ error_module = m.def_submodule("error", "Exception family of SimEstab");
    static nb::exception<error::sim_estab_error> sim_estab_error_exc(error_module, "sim_estab_error",
                                                                     PyExc_RuntimeError);
    return sim_estab_error_exc;
}

void bind_log(nb::module_& m, const nb::exception<error::sim_estab_error>& root) {
    nb::module_ log_module = m.def_submodule("log", "Logging system for SimEstab");

    nb::enum_<estab_log::severity_level>(log_module, "severity_level")
        .value("trace", estab_log::severity_level::trace)
        .value("debug", estab_log::severity_level::debug)
        .value("info", estab_log::severity_level::info)
        .value("warning", estab_log::severity_level::warning)
        .value("error", estab_log::severity_level::error)
        .value("critical", estab_log::severity_level::critical);

    static nb::exception<estab_log::log_error> log_error_exc(log_module, "log_error", root.ptr());

    log_module.def("log_init", &estab_log::log_init, "Initialize the logging system");
    log_module.def("log_deinit", &estab_log::log_deinit, "Deinitialize the logging system");
    log_module.def("log_is_init", &estab_log::log_is_init, "Check if the logging system is initialized");
    log_module.def("enable_console", &estab_log::enable_console, "Enable console logging output");
    log_module.def("disable_console", &estab_log::disable_console, "Disable console logging output");

    // The variadic sim_estab_log gets a concrete wrapper (design_patterns.md §8)
    log_module.def(
        "log",
        [](const std::string& channel, estab_log::severity_level level, const std::string& message) {
            estab_log::sim_estab_log(channel, level, message);
        },
        nb::arg("channel"), nb::arg("level"), nb::arg("message"),
        "Log a message to the SimEstab logging system.\n\n"
        "Args:\n"
        "    channel (str): Hierarchical channel name (e.g., \"sim_estab.network.tcp\")\n"
        "    level (severity_level): Log level (trace, debug, info, warning, error, critical)\n"
        "    message (str): Message content to log");
}

/// The decided limits (design_limits.md), so the Python defaults can be checked against
/// the C++ ones and the two cannot drift
void bind_limits(nb::module_& m) {
    nb::module_ limits_module = m.def_submodule("limits", "Engine-wide limits and policy defaults");

    limits_module.attr("default_tick_rate")                        = limits::default_tick_rate;
    limits_module.attr("default_drain_interval_ticks")             = limits::default_drain_interval_ticks;
    limits_module.attr("default_endpoint_capacity")                = limits::default_endpoint_capacity;
    limits_module.attr("max_endpoint_capacity")                    = limits::max_endpoint_capacity;
    limits_module.attr("default_host_endpoint_capacity")           = limits::default_host_endpoint_capacity;
    limits_module.attr("default_peer_endpoint_capacity")           = limits::default_peer_endpoint_capacity;
    limits_module.attr("default_shutdown_deadline_seconds")        = limits::default_shutdown_deadline_seconds;
    limits_module.attr("max_players")                              = limits::max_players;
    limits_module.attr("max_sources")                              = limits::max_sources;
    limits_module.attr("max_engine_views")                         = limits::max_engine_views;
    limits_module.attr("max_paced_participants")                   = limits::max_paced_participants;
    limits_module.attr("chunk_elements")                           = limits::chunk_elements;
    limits_module.attr("cache_line_bytes")                         = limits::cache_line_bytes;
    limits_module.attr("default_entity_capacity")                  = limits::default_entity_capacity;
    limits_module.attr("max_entity_capacity")                      = limits::max_entity_capacity;
    limits_module.attr("default_projection_warn_bytes_per_second") = limits::default_projection_warn_bytes_per_second;
}

/// The engine's errors, in C++ order: the base before the classes derived from it
void bind_engine_errors(nb::module_& engine_module, const nb::exception<error::sim_estab_error>& root) {
    static nb::exception<engine::engine_error> engine_error_exc(engine_module, "engine_error", root.ptr());
    static nb::exception<engine::engine_state_error> engine_state_error_exc(engine_module, "engine_state_error",
                                                                            engine_error_exc.ptr());
    static nb::exception<engine::engine_closed_error> engine_closed_error_exc(engine_module, "engine_closed_error",
                                                                              engine_error_exc.ptr());
    static nb::exception<engine::engine_failed_error> engine_failed_error_exc(engine_module, "engine_failed_error",
                                                                              engine_error_exc.ptr());
    static nb::exception<engine::engine_exists_error> engine_exists_error_exc(engine_module, "engine_exists_error",
                                                                              engine_error_exc.ptr());
    static nb::exception<engine::event_backlog_error> event_backlog_error_exc(engine_module, "event_backlog_error",
                                                                              engine_error_exc.ptr());

    // event_backlog_error carries the tick reached (design_python_api.md §8). This translator
    // is registered after the type's own, so it is tried first, and raises the exception
    // with `tick_reached` set on it.
    nb::register_exception_translator(
        [](const std::exception_ptr& p, void *payload) {
            try {
                std::rethrow_exception(p);
            } catch (const engine::event_backlog_error& e) {
                const nb::handle type(static_cast<PyObject *>(payload));
                nb::object exc           = type(e.what());
                exc.attr("tick_reached") = nb::cast(e.tick_reached());
                PyErr_SetObject(type.ptr(), exc.ptr());
            }
        },
        event_backlog_error_exc.ptr());
}

void bind_engine_enums(nb::module_& engine_module) {
    nb::enum_<engine::EngineState>(engine_module, "EngineState", "The engine state (design_python_api.md §2)")
        .value("Configuring", engine::EngineState::Configuring)
        .value("Running", engine::EngineState::Running)
        .value("Stopped", engine::EngineState::Stopped)
        .value("Closing", engine::EngineState::Closing)
        .value("Closed", engine::EngineState::Closed)
        .value("LoadFailed", engine::EngineState::LoadFailed)
        .value("Failed", engine::EngineState::Failed);

    nb::enum_<engine::RowPredicateType>(engine_module, "RowPredicateType",
                                        "The row predicate type an engine view declares (design_engine_core.md §3.4)")
        .value("All", engine::RowPredicateType::All);

    nb::enum_<engine::EventDrain>(engine_module, "EventDrain", "Who empties the event ring (design_python_api.md §3)")
        .value("Owner", engine::EventDrain::Owner)
        .value("Sink", engine::EventDrain::Sink);

    nb::enum_<engine::OnExpiry>(engine_module, "OnExpiry",
                                "What a participant's passed deadline does (design_engine_core.md §3.3)")
        .value("Fail", engine::OnExpiry::Fail)
        .value("ContinueWithout", engine::OnExpiry::ContinueWithout);

    nb::enum_<engine::EngineEventType>(engine_module, "EngineEventType",
                                       "What an event reports (design_python_api.md §7.3)")
        .value("SessionStarted", engine::EngineEventType::SessionStarted)
        .value("SessionStopped", engine::EngineEventType::SessionStopped)
        .value("SimBacklogPaused", engine::EngineEventType::SimBacklogPaused)
        .value("SimBacklogResumed", engine::EngineEventType::SimBacklogResumed)
        .value("ParticipantExpired", engine::EngineEventType::ParticipantExpired)
        .value("CommandProtocolError", engine::EngineEventType::CommandProtocolError);

    nb::enum_<engine::Rejection>(engine_module, "Rejection",
                                 "Why submit() refused a command (design_python_api.md §7.1)")
        .value("QueueFull", engine::Rejection::QueueFull)
        .value("TooLate", engine::Rejection::TooLate)
        .value("OutOfOrder", engine::Rejection::OutOfOrder)
        .value("OverMargin", engine::Rejection::OverMargin)
        .value("Revoked", engine::Rejection::Revoked);

    nb::enum_<engine::CommandOutcomeType>(engine_module, "CommandOutcomeType",
                                          "What happened to a command (design_python_api.md §7.1)")
        .value("Pending", engine::CommandOutcomeType::Pending)
        .value("Applied", engine::CommandOutcomeType::Applied)
        .value("Dropped", engine::CommandOutcomeType::Dropped)
        .value("Expired", engine::CommandOutcomeType::Expired);

    nb::enum_<engine::StepEnd>(engine_module, "StepEnd", "What ended a step, or refused it")
        .value("GrantEnded", engine::StepEnd::GrantEnded)
        .value("Stopped", engine::StepEnd::Stopped)
        .value("EventBacklog", engine::StepEnd::EventBacklog)
        .value("Interrupted", engine::StepEnd::Interrupted)
        .value("Failed", engine::StepEnd::Failed)
        .value("StepInFlight", engine::StepEnd::StepInFlight)
        .value("Overflow", engine::StepEnd::Overflow);
}

void bind_engine_values(nb::module_& engine_module) {
    nb::class_<engine::Tick>(engine_module, "Tick",
                             "A tick: one step of the simulation clock (design_engine_core.md §3)")
        .def(nb::init<>())
        .def(
            "__init__", [](engine::Tick *self, std::uint64_t value) { new (self) engine::Tick{value}; },
            nb::arg("value"))
        .def_rw("value", &engine::Tick::value)
        // Field-wise, not through the defaulted operator<=>: GCC 16 crashes mangling a
        // module-attached defaulted comparison that an importing unit instantiates
        .def(
            "__eq__", [](engine::Tick a, engine::Tick b) { return a.value == b.value; }, nb::is_operator())
        .def(
            "__ne__", [](engine::Tick a, engine::Tick b) { return a.value != b.value; }, nb::is_operator())
        .def(
            "__lt__", [](engine::Tick a, engine::Tick b) { return a.value < b.value; }, nb::is_operator())
        .def(
            "__le__", [](engine::Tick a, engine::Tick b) { return a.value <= b.value; }, nb::is_operator())
        .def(
            "__gt__", [](engine::Tick a, engine::Tick b) { return a.value > b.value; }, nb::is_operator())
        .def(
            "__ge__", [](engine::Tick a, engine::Tick b) { return a.value >= b.value; }, nb::is_operator())
        .def("__hash__", [](engine::Tick tick) { return std::hash<std::uint64_t>{}(tick.value); })
        .def("__int__", [](engine::Tick tick) { return tick.value; })
        .def("__index__", [](engine::Tick tick) { return tick.value; })
        .def("__repr__", [](engine::Tick tick) { return "Tick(" + std::to_string(tick.value) + ")"; });

    nb::class_<engine::EngineConfig>(engine_module, "EngineConfig",
                                     "The engine's options: the C++ mirror of the Python EngineConfig, "
                                     "converted once at the boundary (design_python_api.md §3)")
        .def(nb::init<>())
        .def_rw("tick_rate", &engine::EngineConfig::tick_rate)
        .def_rw("seed", &engine::EngineConfig::seed)
        .def_rw("event_drain", &engine::EngineConfig::event_drain)
        .def_rw("drain_interval_ticks", &engine::EngineConfig::drain_interval_ticks)
        .def_rw("endpoint_capacity", &engine::EngineConfig::endpoint_capacity)
        .def_rw("host_endpoint_capacity", &engine::EngineConfig::host_endpoint_capacity)
        .def_rw("peer_endpoint_capacity", &engine::EngineConfig::peer_endpoint_capacity)
        .def_rw("shutdown_deadline", &engine::EngineConfig::shutdown_deadline,
                "Each blocking shutdown stage's budget: a timedelta, or a float of seconds");

    nb::class_<engine::EngineViewSpec>(engine_module, "EngineViewSpec",
                                       "What an engine view declares at registration (design_engine_core.md §3.1)")
        .def(nb::init<>())
        .def_rw("name", &engine::EngineViewSpec::name)
        .def_rw("max_payload_bytes", &engine::EngineViewSpec::max_payload_bytes)
        .def_rw("identity", &engine::EngineViewSpec::identity)
        .def_rw("predicate", &engine::EngineViewSpec::predicate)
        .def_rw("cadence", &engine::EngineViewSpec::cadence)
        .def_rw("paced", &engine::EngineViewSpec::paced)
        .def_rw("deadline_ms", &engine::EngineViewSpec::deadline_ms)
        .def_rw("on_expiry", &engine::EngineViewSpec::on_expiry);

    nb::class_<engine::SinkDigest>(engine_module, "SinkDigest",
                                   "What the sink drain has consumed (design_engine_core.md §5.2)")
        .def_ro("count", &engine::SinkDigest::count);

    nb::class_<engine::StepResult>(engine_module, "StepResult", "How a step ended")
        .def_ro("end", &engine::StepResult::end)
        .def_ro("tick_reached", &engine::StepResult::tick_reached)
        .def_ro("participant", &engine::StepResult::participant)
        .def_ro("interrupted_by", &engine::StepResult::interrupted_by);

    engine_module.attr("provisional_command_payload_bytes") = engine::provisional_command_payload_bytes;

    nb::class_<engine::Command>(engine_module, "Command",
                                "One command: the tick it acts on and its opaque payload "
                                "(design_engine_core.md §5.1, §7 step 1)")
        .def(nb::init<>())
        .def_rw("tick", &engine::Command::tick)
        .def_prop_rw(
            "payload",
            [](const engine::Command& command) { return nb::bytes(command.payload.data(), command.payload.size()); },
            [](engine::Command& command, const nb::bytes& payload) {
                if (payload.size() > command.payload.size()) {
                    throw nb::value_error(("a command payload holds at most " + std::to_string(command.payload.size()) +
                                           " bytes, got " + std::to_string(payload.size()))
                                              .c_str());
                }
                command.payload.fill(std::byte{0});
                std::memcpy(command.payload.data(), payload.data(), payload.size());
            },
            "The payload, zero-padded to provisional_command_payload_bytes")
        .def(
            "__eq__",
            [](const engine::Command& a, const engine::Command& b) {
                return a.tick == b.tick && a.payload == b.payload;
            },
            nb::is_operator())
        .def(
            "__ne__",
            [](const engine::Command& a, const engine::Command& b) {
                return a.tick != b.tick || a.payload != b.payload;
            },
            nb::is_operator());

    nb::class_<engine::CommandHandle>(engine_module, "CommandHandle", "What a producer reads a command's outcome with")
        .def(nb::init<>())
        .def_rw("source", &engine::CommandHandle::source)
        .def_rw("position", &engine::CommandHandle::position)
        .def(
            "__eq__",
            [](const engine::CommandHandle& a, const engine::CommandHandle& b) {
                return a.source == b.source && a.position == b.position;
            },
            nb::is_operator())
        .def(
            "__ne__",
            [](const engine::CommandHandle& a, const engine::CommandHandle& b) {
                return a.source != b.source || a.position != b.position;
            },
            nb::is_operator())
        .def("__hash__",
             [](const engine::CommandHandle& handle) {
                 return std::hash<std::uint64_t>{}(handle.position) ^ (std::hash<std::uint32_t>{}(handle.source) << 1);
             })
        .def("__repr__", [](const engine::CommandHandle& handle) {
            return "CommandHandle(source=" + std::to_string(handle.source) +
                   ", position=" + std::to_string(handle.position) + ")";
        });

    nb::class_<engine::CommandOutcome>(engine_module, "CommandOutcome",
                                       "A command's outcome, and the tick it concerns (design_python_api.md §7.1)")
        .def_ro("type", &engine::CommandOutcome::type)
        .def_ro("tick", &engine::CommandOutcome::tick)
        .def(
            "__eq__",
            [](const engine::CommandOutcome& a, const engine::CommandOutcome& b) {
                return a.type == b.type && a.tick.value == b.tick.value;
            },
            nb::is_operator())
        .def(
            "__ne__",
            [](const engine::CommandOutcome& a, const engine::CommandOutcome& b) {
                return a.type != b.type || a.tick.value != b.tick.value;
            },
            nb::is_operator())
        .def("__repr__", [](const engine::CommandOutcome& outcome) {
            return "CommandOutcome(type=" + nb::cast<std::string>(nb::cast(outcome.type).attr("name")) +
                   ", tick=" + std::to_string(outcome.tick.value) + ")";
        });

    nb::class_<engine::EngineEvent>(engine_module, "EngineEvent", "One event (design_engine_core.md §5.2)")
        .def_ro("tick", &engine::EngineEvent::tick)
        .def_ro("type", &engine::EngineEvent::type)
        .def_ro("subject", &engine::EngineEvent::subject)
        .def_ro("count", &engine::EngineEvent::count)
        .def(
            "__eq__",
            [](const engine::EngineEvent& a, const engine::EngineEvent& b) {
                return a.tick == b.tick && a.type == b.type && a.subject == b.subject && a.count == b.count;
            },
            nb::is_operator())
        .def(
            "__ne__",
            [](const engine::EngineEvent& a, const engine::EngineEvent& b) {
                return a.tick != b.tick || a.type != b.type || a.subject != b.subject || a.count != b.count;
            },
            nb::is_operator())
        .def("__repr__", [](const engine::EngineEvent& event) {
            return "EngineEvent(tick=" + std::to_string(event.tick) +
                   ", type=" + nb::cast<std::string>(nb::cast(event.type).attr("name")) +
                   ", subject=" + std::to_string(event.subject) + ", count=" + std::to_string(event.count) + ")";
        });

    nb::class_<engine::Block>(engine_module, "Block",
                              "A block an engine view holds: the tick it shows and its payload "
                              "(design_engine_core.md §3.1)")
        .def_ro("tick", &engine::Block::tick)
        .def_ro("size", &engine::Block::size)
        .def_prop_ro(
            "payload",
            [](const engine::Block& block) {
                return block.size == 0 ? nb::bytes("") : nb::bytes(block.payload.get(), block.size);
            },
            "The payload bytes in use");
}

void bind_engine_classes(nb::module_& engine_module) {
    nb::class_<engine::EngineViewHandle>(engine_module, "EngineViewHandle",
                                         "The reader's handle on an engine view (design_engine_core.md §3.1), "
                                         "from Engine.view(). It belongs to the engine view's one reader thread.")
        .def("take", &engine::EngineViewHandle::take,
             "Take the newest published block, handing back the one held. True if a newer block "
             "was taken; on a paced engine view the take is the reader's declaration.")
        .def(
            "held",
            [](const engine::EngineViewHandle& handle) {
                // A copy, so the Python object never outlives the block it shows: the block
                // is written by the next take() and freed by close(). Zero-copy arrives with
                // projection (M5), under the detach rule of design_python_api.md §7.2.
                const engine::Block& held = handle.held();
                engine::Block copy;
                copy.tick = held.tick;
                copy.size = held.size;
                if (held.size > 0) {
                    copy.payload = std::make_unique<std::byte[]>(held.size);
                    std::memcpy(copy.payload.get(), held.payload.get(), held.size);
                }
                return copy;
            },
            "A copy of the block it holds: a snapshot once the first take() has succeeded")
        .def("name", &engine::EngineViewHandle::name)
        .def("cadence", &engine::EngineViewHandle::cadence)
        .def("is_paced", &engine::EngineViewHandle::is_paced);

    nb::class_<engine::Engine>(engine_module, "Engine",
                               "The engine (design_python_api.md §2): one per process. Every call but "
                               "close(), state() and config() raises engine_closed_error once close() has "
                               "begun, and engine_failed_error once the engine is Failed.")
        .def(
            "__init__",
            [](engine::Engine *self, const engine::EngineConfig& config) {
                engine::StepCallbacks step_callbacks;
                step_callbacks["signal_check"] = &signal_check;
                new (self) engine::Engine(config, std::move(step_callbacks));
            },
            nb::arg("config"),
            "Acquire, and leave the engine Configuring. The binding registers one step callback, "
            "the signal check, so a KeyboardInterrupt ends a step at a tick boundary.")
        .def("state", &engine::Engine::state, "Any thread: the engine state")
        .def("config", &engine::Engine::config, "The config the engine was constructed with")
        .def("seed", &engine::Engine::seed, "Running or Stopped: the seed the freeze resolved")
        .def("register_engine_view", &engine::Engine::register_engine_view, nb::arg("spec"),
             "Configuring: declare an engine view (design_engine_core.md §3.1)")
        .def("start_session", &engine::Engine::start_session, "Configuring: the freeze (design_engine_core.md §2.4)")
        .def("step", &engine::Engine::step, nb::arg("tick_count"), nb::call_guard<nb::gil_scoped_release>(),
             "Running: run tick_count ticks on the calling thread, with the GIL released "
             "(design_python_api.md §4.3). Returns the tick reached and what ended the step.")
        .def(
            "submit",
            [](engine::Engine& self, const engine::Command& command) -> nb::object {
                const auto result = self.submit(command);
                return result ? nb::cast(*result) : nb::cast(result.error());
            },
            nb::arg("command"),
            "Running or Stopped: submit a command on the host endpoint. Never waits: returns the "
            "CommandHandle, or the Rejection (design_python_api.md §7.1).")
        .def(
            "submit_batch",
            [](engine::Engine& self, const std::vector<engine::Command>& commands) {
                // One crossing per batch (design_python_api.md §5 rule 1); one answer per
                // command, in array order
                nb::list results;
                for (const engine::Command& command : commands) {
                    const auto result = self.submit(command);
                    results.append(result ? nb::cast(*result) : nb::cast(result.error()));
                }
                return results;
            },
            nb::arg("commands"),
            "Running or Stopped: submit commands on the host endpoint, in order; one CommandHandle "
            "or Rejection per command")
        .def("outcome", &engine::Engine::outcome, nb::arg("handle"),
             "Running or Stopped: a host command's outcome. Never waits.")
        .def("view", &engine::Engine::view, nb::arg("name"),
             "Running or Stopped: the handle of the engine view registered as name")
        .def(
            "drain_events",
            [](engine::Engine& self) {
                std::vector<engine::EngineEvent> events;
                self.drain_events(events);
                return events;
            },
            "Running or Stopped, owner drain: every event raised so far, in tick order. Never blocks.")
        .def("sink_digest", &engine::Engine::sink_digest, "Running or Stopped, sink drain: what the sink has consumed")
        .def("snapshot", &engine::Engine::snapshot,
             "Running or Stopped, while no step is in flight: the last tick that has run")
        .def("pause", &engine::Engine::pause, "Any thread: no tick runs after the one in progress")
        .def("resume", &engine::Engine::resume, "Any thread: lift the ceiling on the run grant")
        .def("request_stop", &engine::Engine::request_stop, "Any thread: no tick runs again in this session")
        .def("stop_requested", &engine::Engine::stop_requested, "Any thread: whether a stop was requested")
        .def("stop_sim_async", &engine::Engine::stop_sim_async, nb::call_guard<nb::gil_scoped_release>(),
             "Running: end ticking for the session; the engine is Stopped")
        .def("close", &engine::Engine::close, nb::call_guard<nb::gil_scoped_release>(),
             "Owner thread: shut down, with the GIL released (design_python_api.md §2 step 3). "
             "Never raises for a shutdown failure, and a second call does nothing.");
}

void bind_engine(nb::module_& m, const nb::exception<error::sim_estab_error>& root) {
    nb::module_ engine_module = m.def_submodule("engine", "The engine: the session, the executor and the boundary");
    bind_engine_errors(engine_module, root);
    bind_engine_enums(engine_module);
    bind_engine_values(engine_module);
    bind_engine_classes(engine_module);
}

} // namespace

NB_MODULE(_if, m) {
    const nb::exception<error::sim_estab_error>& root = bind_error(m);
    bind_log(m, root);
    bind_limits(m);
    bind_engine(m, root);
}
