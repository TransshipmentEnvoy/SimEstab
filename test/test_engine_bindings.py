#!/usr/bin/env python3

"""Tests for the bindings in ``sim_estab._if.engine`` and ``sim_estab._if.limits``.

``_if`` is the 1:1 mirror of the bound C++ API (``design_patterns.md`` §8): a bound name
keeps its C++ spelling, and the mirror alone can run a session. The two checks a
functional test can make of the threading rules are here too: ``step()`` releases the
GIL, and the signal check ends a step at a tick boundary (``design_python_api.md``
§4.3, §5 rule 2).
"""

import datetime
import signal
import threading
import time

import pytest

import sim_estab._if as if_module

engine_module = if_module.engine


def sink_config():
    config = engine_module.EngineConfig()
    config.event_drain = engine_module.EventDrain.Sink
    return config


@pytest.fixture
def engine():
    """A fresh engine, closed after the test: one lives at a time."""
    native = engine_module.Engine(sink_config())
    try:
        yield native
    finally:
        native.close()


# --------------------------------------------------------------------------
# the mirror
# --------------------------------------------------------------------------


def test_submodules_mirror_the_partitions():
    """A partition ``sim_estab:<p>`` is the submodule ``_if.<p>``."""
    assert hasattr(if_module, "engine")
    assert hasattr(if_module, "limits")
    for name in (
        "Engine",
        "EngineConfig",
        "EngineViewSpec",
        "EngineViewHandle",
        "EngineState",
        "StepResult",
        "StepEnd",
        "Tick",
        "Command",
        "CommandHandle",
        "CommandOutcome",
        "CommandOutcomeType",
        "Rejection",
        "EngineEvent",
        "EngineEventType",
        "EventDrain",
        "OnExpiry",
        "RowPredicateType",
        "SinkDigest",
        "Block",
        "provisional_command_payload_bytes",
    ):
        assert hasattr(engine_module, name), name
    # the placeholders the lifecycle replaced are gone (design_patterns.md §8)
    for placeholder in ("init", "deinit", "run"):
        assert not hasattr(if_module, placeholder), placeholder


def test_bound_names_keep_their_cpp_spelling():
    """Enumerators and exceptions keep the C++ spelling (``design_patterns.md`` §10)."""
    assert engine_module.EngineState.Configuring.name == "Configuring"
    assert engine_module.Rejection.QueueFull.name == "QueueFull"
    assert engine_module.CommandOutcomeType.Pending.name == "Pending"
    assert engine_module.EngineEventType.SessionStarted.name == "SessionStarted"
    assert engine_module.OnExpiry.ContinueWithout.name == "ContinueWithout"
    assert engine_module.engine_closed_error.__name__ == "engine_closed_error"


def test_errors_form_one_family():
    """Every engine error derives from ``engine_error``, which derives from the root
    (``design_python_api.md`` §8)."""
    root = if_module.error.sim_estab_error
    assert issubclass(engine_module.engine_error, root)
    for name in (
        "engine_state_error",
        "engine_closed_error",
        "engine_failed_error",
        "engine_exists_error",
        "event_backlog_error",
    ):
        assert issubclass(
            getattr(engine_module, name), engine_module.engine_error
        ), name


def test_limits_are_the_cpp_constants():
    limits = if_module.limits
    assert limits.default_tick_rate == 30
    assert limits.max_endpoint_capacity == 256
    assert limits.default_shutdown_deadline_seconds == 5.0
    assert limits.max_engine_views == 64
    assert limits.cache_line_bytes == 64


# --------------------------------------------------------------------------
# the value types
# --------------------------------------------------------------------------


def test_tick_is_a_strong_type():
    Tick = engine_module.Tick
    assert Tick().value == 0
    assert Tick(5) == Tick(5)
    assert Tick(5) != Tick(6)
    assert Tick(3) < Tick(4) <= Tick(4) < Tick(9)
    assert Tick(9) > Tick(4) >= Tick(4)
    assert int(Tick(7)) == 7
    assert list(range(Tick(3))) == [0, 1, 2]  # __index__
    assert hash(Tick(7)) == hash(Tick(7))
    assert repr(Tick(7)) == "Tick(7)"
    assert Tick(1) != 1  # a bare integer is not a tick


def test_command_payload_is_bounded_and_zero_padded():
    size = engine_module.provisional_command_payload_bytes
    command = engine_module.Command()
    assert command.tick == 0
    assert command.payload == bytes(size)
    command.payload = b"abc"
    assert command.payload == b"abc" + bytes(size - 3)
    with pytest.raises(ValueError, match="at most"):
        command.payload = bytes(size + 1)
    same = engine_module.Command()
    same.payload = b"abc"
    assert command == same
    same.tick = 1
    assert command != same


def test_shutdown_deadline_is_a_duration():
    config = engine_module.EngineConfig()
    assert config.shutdown_deadline == datetime.timedelta(seconds=5)
    config.shutdown_deadline = 0.25  # a float is seconds
    assert config.shutdown_deadline == datetime.timedelta(milliseconds=250)
    config.shutdown_deadline = datetime.timedelta(seconds=2)
    assert config.shutdown_deadline == datetime.timedelta(seconds=2)


def test_config_optionals_accept_none():
    config = engine_module.EngineConfig()
    assert config.seed is None
    assert config.event_drain is None
    config.seed = 42
    config.event_drain = engine_module.EventDrain.Owner
    assert config.seed == 42
    assert config.event_drain == engine_module.EventDrain.Owner
    config.seed = None
    assert config.seed is None


# --------------------------------------------------------------------------
# a session through the mirror alone
# --------------------------------------------------------------------------


def test_a_session_through_the_mirror(engine):
    assert engine.state() == engine_module.EngineState.Configuring
    spec = engine_module.EngineViewSpec()
    spec.name = "ui"
    spec.cadence = 2
    engine.register_engine_view(spec)
    engine.start_session()
    assert engine.state() == engine_module.EngineState.Running
    assert engine.snapshot() == engine_module.Tick(0)

    command = engine_module.Command()
    handle = engine.submit(command)
    assert isinstance(handle, engine_module.CommandHandle)
    assert engine.outcome(handle).type == engine_module.CommandOutcomeType.Pending

    result = engine.step(5)
    assert result.end == engine_module.StepEnd.GrantEnded
    assert result.tick_reached == engine_module.Tick(5)
    assert result.interrupted_by == ""
    outcome = engine.outcome(handle)
    assert outcome.type == engine_module.CommandOutcomeType.Applied
    assert outcome.tick == engine_module.Tick(1)

    handles = engine.submit_batch([engine_module.Command(), engine_module.Command()])
    assert [h.position for h in handles] == [1, 2]

    view = engine.view("ui")
    assert view.name() == "ui"
    assert view.cadence() == 2
    assert not view.is_paced()
    assert view.take()
    assert view.held().tick == 4  # the last publish due by cadence 2
    assert view.held().payload == b""
    assert not view.take()  # nothing newer

    assert engine.sink_digest().count >= 1  # session.started at least

    engine.request_stop()
    result = engine.step(5)
    assert result.end == engine_module.StepEnd.Stopped
    assert engine.state() == engine_module.EngineState.Stopped

    engine.close()
    assert engine.state() == engine_module.EngineState.Closed
    engine.close()  # idempotent
    with pytest.raises(engine_module.engine_closed_error):
        engine.snapshot()
    with pytest.raises(engine_module.engine_closed_error):
        view.take()
    assert view.name() == "ui"  # plain data, no engine behind it


def test_owner_drain_hands_out_the_events(engine):
    config = engine_module.EngineConfig()
    config.event_drain = engine_module.EventDrain.Owner
    engine.close()
    owner = engine_module.Engine(config)
    try:
        owner.start_session()
        events = owner.drain_events()
        assert [e.type for e in events] == [
            engine_module.EngineEventType.SessionStarted
        ]
        assert owner.drain_events() == []
        with pytest.raises(engine_module.engine_state_error):
            owner.sink_digest()
    finally:
        owner.close()


def test_a_live_engine_is_one_per_process(engine):
    """A second construction raises while one lives; a closed engine is not live
    (``design_python_api.md`` §2)."""
    with pytest.raises(engine_module.engine_exists_error):
        engine_module.Engine(sink_config())
    engine.close()
    second = engine_module.Engine(sink_config())  # the closed one still exists
    second.close()


# --------------------------------------------------------------------------
# the threading rules a functional test can check
# --------------------------------------------------------------------------


def test_step_releases_the_gil(engine):
    """A control call from another thread ends a long step: with the GIL held through
    the step, the thread could never make it (``design_python_api.md`` §5 rule 2)."""
    engine.start_session()
    timer = threading.Timer(0.1, engine.request_stop)
    timer.start()
    started = time.monotonic()
    try:
        result = engine.step(10**9)
    finally:
        timer.cancel()
    assert result.end == engine_module.StepEnd.Stopped
    assert time.monotonic() - started < 10.0


def test_a_keyboard_interrupt_ends_the_step_at_a_tick_boundary(engine):
    """The signal check runs between parks at the gate; the interrupt revokes the grant
    and propagates, and no tick is cut short (``design_python_api.md`` §4.3)."""
    spec = engine_module.EngineViewSpec()
    spec.name = "recorder"
    spec.paced = True
    spec.deadline_ms = 20_000
    spec.on_expiry = engine_module.OnExpiry.ContinueWithout
    engine.register_engine_view(spec)
    engine.start_session()
    recorder = engine.view("recorder")
    assert recorder.take()  # tick 0: ready through tick 1

    # Nobody takes again, so the step parks at the gate for tick 2, and the pending
    # SIGINT is found by the signal check between parks.
    timer = threading.Timer(0.3, signal.raise_signal, [signal.SIGINT])
    timer.start()
    started = time.monotonic()
    try:
        with pytest.raises(KeyboardInterrupt):
            engine.step(10)
    finally:
        timer.cancel()
    assert time.monotonic() - started < 10.0
    assert engine.state() == engine_module.EngineState.Running
    assert engine.snapshot() == engine_module.Tick(1)  # whole ticks only
