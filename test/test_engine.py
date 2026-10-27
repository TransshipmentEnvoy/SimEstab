#!/usr/bin/env python3

"""Tests for the Python ``Engine`` (``design_python_api.md`` §2, §4, §6, §8): the
lifecycle and its states, the context manager, the owner-thread check, the Python-style
error names, commands and engine views through the Python layer, logging held around
the engine's life, and ``on_failed_stop``.

A ``Failed`` engine poisons its process, so those cases run in a subprocess.
"""

import gc
import signal
import subprocess
import sys
import threading

import pytest

import sim_estab
from sim_estab import (
    CommandHandle,
    CommandOutcomeType,
    Engine,
    EngineClosedError,
    EngineConfig,
    EngineError,
    EngineExistsError,
    EngineState,
    EngineStateError,
    EngineThreadError,
    Rejection,
    StepEnd,
    Tick,
    noop,
)
from sim_estab.upkeep import log as sim_log


def sink_config(**overrides):
    return EngineConfig(headless=True, event_drain="sink", **overrides)


@pytest.fixture
def engine():
    """A fresh engine, closed after the test: one lives at a time."""
    instance = Engine(sink_config())
    try:
        yield instance
    finally:
        instance.close()


# --------------------------------------------------------------------------
# the lifecycle
# --------------------------------------------------------------------------


def test_set_log_level_is_a_control_call(engine):
    """``set_log_level`` forwards to the subsystem's ``set_severity`` from any thread
    and validates its string (``design_logging.md`` §2, §3)."""
    engine.set_log_level("debug")
    done = threading.Event()

    def from_another_thread() -> None:
        engine.set_log_level("info")
        done.set()

    threading.Thread(target=from_another_thread).start()
    assert done.wait(5)
    with pytest.raises(ValueError, match="level must be one of"):
        engine.set_log_level("bogus")


def test_set_log_level_on_a_closed_engine_raises():
    """A closed engine never changes the level, which an application may still hold
    (``design_python_api.md`` §8)."""
    closed = Engine(sink_config())
    closed.close()
    with pytest.raises(EngineClosedError):
        closed.set_log_level("trace")


def test_the_states_through_a_session(engine):
    assert engine.state == EngineState.Configuring
    engine.start_session()
    assert engine.state == EngineState.Running
    assert engine.seed() >= 0

    result = engine.step(3)
    assert result.end == StepEnd.GrantEnded
    assert result.tick_reached == Tick(3)
    assert engine.snapshot() == Tick(3)

    engine.stop_sim_async()
    assert engine.state == EngineState.Stopped
    with pytest.raises(EngineStateError):
        engine.step(1)
    assert engine.snapshot() == Tick(3)  # reads still work

    engine.close()
    assert engine.state == EngineState.Closed
    with pytest.raises(EngineClosedError):
        engine.snapshot()
    engine.close()  # idempotent


def test_the_context_manager_stops_and_closes():
    with Engine(sink_config()) as engine:
        engine.start_session()
        engine.step(2)
    assert engine.state == EngineState.Closed


def test_a_body_exception_is_never_masked():
    with pytest.raises(ZeroDivisionError):
        with Engine(sink_config()) as engine:
            engine.start_session()
            1 / 0
    assert engine.state == EngineState.Closed


def test_the_seed_is_kept():
    with Engine(sink_config(seed=7)) as engine:
        engine.start_session()
        assert engine.seed() == 7


def test_the_error_names_are_the_bound_types():
    """``sim_estab.EngineClosedError`` is ``_if.engine.engine_closed_error``, so
    ``except`` works with either spelling."""
    assert EngineClosedError is sim_estab._if.engine.engine_closed_error
    assert sim_estab.EventBacklogError is sim_estab._if.engine.event_backlog_error
    assert issubclass(EngineThreadError, EngineError)
    assert issubclass(EngineError, sim_estab.SimEstabError)
    assert issubclass(sim_estab.SimEstabError, RuntimeError)


def test_a_windowed_session_is_not_built_yet():
    held = sim_log.holder_count()
    with pytest.raises(NotImplementedError, match="headless=True"):
        Engine(EngineConfig(headless=False, event_drain="sink"))
    assert sim_log.holder_count() == held  # refused before anything was touched


def test_config_must_be_an_engine_config():
    with pytest.raises(TypeError):
        Engine({"headless": True})  # type: ignore[arg-type]


def test_the_freeze_needs_an_event_drain():
    with Engine(EngineConfig(headless=True)) as engine:
        with pytest.raises(ValueError, match="event_drain"):
            engine.start_session()
        assert engine.state == EngineState.Configuring


# --------------------------------------------------------------------------
# the thread contract
# --------------------------------------------------------------------------


def test_lifecycle_and_pump_calls_are_owner_thread_only(engine):
    """Off the owner thread they raise ``EngineThreadError`` and touch nothing; the
    control calls work from any thread (``design_python_api.md`` §6)."""
    engine.start_session()
    outcomes = []

    def off_owner() -> None:
        calls = (
            lambda: engine.step(1),
            lambda: engine.submit(noop()),
            engine.snapshot,
            engine.sink_digest,
            engine.start_session,
            engine.stop_sim_async,
            engine.close,
        )
        for call in calls:
            try:
                call()
            except EngineThreadError as error:
                outcomes.append(type(error))
            else:
                outcomes.append(None)
        engine.pause()
        engine.resume()
        engine.request_stop()
        outcomes.append(engine.stop_requested())

    thread = threading.Thread(target=off_owner)
    thread.start()
    thread.join()
    assert outcomes == [EngineThreadError] * 7 + [True]
    assert (
        engine.state == EngineState.Running
    )  # close() off the owner thread did nothing
    assert engine.stop_requested()


# --------------------------------------------------------------------------
# registration, commands and engine views
# --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "kwargs",
    [
        {"predicate": "sphere"},
        {"on_expiry": "retry"},
        {"cadence": 0},
        {"deadline_ms": -1},
        {"max_payload_bytes": -1},
    ],
)
def test_register_engine_view_validates_before_native(engine, kwargs):
    with pytest.raises(ValueError):
        engine.register_engine_view("ui", **kwargs)


def test_register_engine_view_is_a_configuring_call(engine):
    engine.register_engine_view("ui")
    with pytest.raises(EngineError):
        engine.register_engine_view("ui")  # a duplicate name
    engine.start_session()
    with pytest.raises(EngineStateError):
        engine.register_engine_view("late")


def test_commands_get_two_answers(engine):
    """Admission is ``submit``'s return; the outcome is read from the handle, and
    exists once the tick that drained the command has run (``design_python_api.md``
    §7.1)."""
    engine.start_session()
    handle = engine.submit(noop(b"spawn"))
    assert isinstance(handle, CommandHandle)
    assert engine.outcome(handle).type == CommandOutcomeType.Pending

    engine.step(1)
    outcome = engine.outcome(handle)
    assert outcome.type == CommandOutcomeType.Applied
    assert outcome.tick == Tick(1)

    handles = engine.submit_batch(noop() for _ in range(3))
    assert all(isinstance(h, CommandHandle) for h in handles)
    assert [o.type for o in engine.outcomes(handles)] == [
        CommandOutcomeType.Pending
    ] * 3
    engine.step(1)
    assert {o.tick for o in engine.outcomes(handles)} == {Tick(2)}


def test_a_full_host_endpoint_returns_queue_full(engine):
    """The host endpoint never waits: beyond its capacity the answer is ``QueueFull``,
    returned, never raised (``design_python_api.md`` §7.1)."""
    engine.start_session()
    capacity = engine.config.command_policy.host_endpoint_capacity
    answers = engine.submit_batch(noop() for _ in range(capacity + 1))
    assert all(isinstance(a, CommandHandle) for a in answers[:capacity])
    assert answers[capacity] == Rejection.QueueFull
    engine.step(1)  # the drain made room
    assert isinstance(engine.submit(noop()), CommandHandle)


def test_the_engine_view_handle(engine):
    engine.register_engine_view("ui", cadence=2, max_payload_bytes=8)
    engine.start_session()
    ui = engine.view("ui")
    assert (ui.name, ui.cadence, ui.paced) == ("ui", 2, False)
    assert ui.take()  # tick 0, published at the freeze
    assert ui.held().tick == 0
    engine.step(3)
    assert ui.take()
    assert ui.held().tick == 2  # the last publish due by cadence 2
    assert not ui.take()  # nothing newer
    with pytest.raises(EngineError):
        engine.view("nope")


def test_a_paced_engine_view_declares_by_taking(engine):
    """Taking the block of tick ``t`` declares the reader ready through ``t + cadence``;
    a step past that waits for a take, and after the deadline continues without the
    reader (``design_python_api.md`` §7.2)."""
    engine.register_engine_view(
        "recorder", paced=True, deadline_ms=200, on_expiry="continue_without"
    )
    engine.start_session()
    recorder = engine.view("recorder")
    assert recorder.paced
    assert recorder.take()  # tick 0: ready through tick 1
    assert engine.step(1).tick_reached == Tick(1)
    result = engine.step(2)  # tick 2 waits 200 ms, then the reader is left behind
    assert result.end == StepEnd.GrantEnded
    assert result.tick_reached == Tick(3)


def test_a_handle_after_close_raises_closed(engine):
    engine.register_engine_view("ui")
    engine.start_session()
    ui = engine.view("ui")
    engine.close()
    with pytest.raises(EngineClosedError):
        ui.take()
    with pytest.raises(EngineClosedError):
        ui.held()
    assert ui.name == "ui"  # plain data, no engine behind it


# --------------------------------------------------------------------------
# logging around the engine's life, and the finalizer
# --------------------------------------------------------------------------


def test_logging_is_held_around_the_engine():
    """Acquired before the native construction, released once, on the first close()
    (``design_python_api.md`` §2)."""
    assert sim_log.holder_count() == 0
    engine = Engine(sink_config())
    assert sim_log.holder_count() == 1
    with pytest.raises(EngineExistsError):
        Engine(sink_config())
    assert sim_log.holder_count() == 1  # the failed construction let go of its hold
    engine.close()
    assert sim_log.holder_count() == 0
    engine.close()
    assert sim_log.holder_count() == 0


def test_a_closed_engine_is_not_live():
    first = Engine(sink_config())
    first.close()
    with Engine(sink_config()) as second:  # `first` still exists
        second.start_session()
        assert second.step(1).tick_reached == Tick(1)
    with pytest.raises(EngineClosedError):
        first.snapshot()


def test_the_finalizer_warns_and_closes():
    engine = Engine(sink_config())
    engine.start_session()
    with pytest.warns(ResourceWarning, match="never closed"):
        del engine
        gc.collect()
    assert sim_log.holder_count() == 0
    Engine(sink_config()).close()  # the singleton was released


# --------------------------------------------------------------------------
# on_failed_stop, in a subprocess: a Failed engine poisons its process
# --------------------------------------------------------------------------

FAILED_PATH_SCRIPT = """
import sys
from sim_estab import Engine, EngineConfig, EngineExistsError
from sim_estab import EngineFailedError, EngineState

config = EngineConfig(headless=True, event_drain="sink", on_failed_stop={policy!r})
engine = Engine(config)
engine.register_engine_view("recorder", paced=True, deadline_ms=50, on_expiry="fail")
engine.start_session()
try:
    engine.step(2)  # tick 1 waits for a take that never comes; the deadline passes
except EngineFailedError:
    print("raised", engine.state.name)
engine.close()  # a no-op in Failed
print("closed", engine.state.name)
try:
    Engine(config)
except EngineExistsError:
    print("poisoned")
"""


def run_failed_path(policy: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-c", FAILED_PATH_SCRIPT.format(policy=policy)],
        capture_output=True,
        text=True,
        timeout=120,
    )


def test_on_failed_stop_raise_enters_failed_and_poisons():
    """``"raise"``: the call that hit the path raises ``EngineFailedError``; ``close()``
    is a no-op, and a new ``Engine`` still raises ``EngineExistsError``
    (``design_python_api.md`` §2, §3)."""
    proc = run_failed_path("raise")
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.split() == ["raised", "Failed", "closed", "Failed", "poisoned"]


def test_on_failed_stop_terminate_aborts_the_process():
    """``"terminate"``: abort at the entry path, after a critical log."""
    proc = run_failed_path("terminate")
    assert proc.returncode == -signal.SIGABRT, (proc.returncode, proc.stderr)
    assert "raised" not in proc.stdout
    assert "aborting the process" in proc.stderr
