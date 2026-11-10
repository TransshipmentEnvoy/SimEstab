#!/usr/bin/env python3

"""Tests for the reference headless loop (``design_python_api.md`` §4.2)."""

import signal
import threading

import pytest

from sim_estab import (
    Engine,
    EngineConfig,
    EngineEventType,
    EngineState,
    EventBacklogError,
    Tick,
    run_headless,
)


def test_run_headless_under_a_sink_drain():
    with Engine(EngineConfig(headless=True, event_drain="sink")) as engine:
        engine.start_session()
        assert run_headless(engine, 10) == Tick(10)
        assert run_headless(engine, 5) == Tick(15)
        assert run_headless(engine, 0) == Tick(15)
        assert engine.sink_digest().count >= 1  # the sink emptied the ring itself


def test_run_headless_under_an_owner_drain_hands_the_events_over():
    batches = []
    with Engine(EngineConfig(headless=True, event_drain="owner")) as engine:
        engine.start_session()
        assert run_headless(engine, 4, on_events=batches.append) == Tick(4)
        assert engine.drain_events() == []  # the loop drained when it ended
    assert [e.type for batch in batches for e in batch] == [
        EngineEventType.SessionStarted
    ]


def test_run_headless_drains_at_the_backlog_mark(monkeypatch):
    """A step that raises ``EventBacklogError`` is followed by a drain, and the loop
    steps on from the tick reached. The M1 engine raises no event inside a tick, so the
    mark is simulated."""
    with Engine(EngineConfig(headless=True, event_drain="owner")) as engine:
        engine.start_session()
        real_step = engine.step
        asked = []

        def step_once_backlogged(tick_count):
            asked.append(tick_count)
            if len(asked) == 1:
                real_step(2)  # two ticks ran before the mark
                error = EventBacklogError("the event backlog reached its mark")
                error.tick_reached = Tick(2)
                raise error
            return real_step(tick_count)

        monkeypatch.setattr(engine, "step", step_once_backlogged)
        batches = []
        assert run_headless(engine, 5, on_events=batches.append) == Tick(5)
        assert asked == [5, 3]
        assert len(batches) == 2  # at the mark, and when the loop ended


def test_run_headless_ends_early_at_a_stop():
    with Engine(EngineConfig(headless=True, event_drain="sink")) as engine:
        engine.start_session()
        engine.request_stop()
        assert run_headless(engine, 5) == Tick(0)
        assert engine.state == EngineState.Stopped


def test_run_headless_rejects_a_negative_count():
    with Engine(EngineConfig(headless=True, event_drain="sink")) as engine:
        engine.start_session()
        with pytest.raises(ValueError, match="not be negative"):
            run_headless(engine, -1)


def test_run_headless_drains_once_more_on_a_keyboard_interrupt():
    """A ``KeyboardInterrupt`` ends the step at a tick boundary and propagates
    (``design_python_api.md`` §4.3). Under an owner drain the loop still drains once
    more before it lets the interrupt go on, so no event waits in the ring."""
    batches = []
    with Engine(EngineConfig(headless=True, event_drain="owner")) as engine:
        # A paced reader that never takes again parks the step at the gate for tick 2,
        # where the signal check finds the SIGINT (as in test_engine_bindings.py).
        engine.register_engine_view(
            "recorder", paced=True, deadline_ms=20_000, on_expiry="continue_without"
        )
        engine.start_session()
        assert engine.view("recorder").take()
        timer = threading.Timer(0.3, signal.raise_signal, [signal.SIGINT])
        timer.start()
        try:
            with pytest.raises(KeyboardInterrupt):
                run_headless(engine, 10, on_events=batches.append)
        finally:
            timer.cancel()
        assert engine.state == EngineState.Running
        assert engine.snapshot() == Tick(1)  # whole ticks only
        assert engine.drain_events() == []  # the loop drained before it let go
    assert [e.type for batch in batches for e in batch] == [
        EngineEventType.SessionStarted
    ]
