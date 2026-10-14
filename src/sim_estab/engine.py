"""The Engine: the session lifecycle and the pump primitives (``design_python_api.md``
§2, §4).

One ``Engine`` lives per process. Construction acquires native resources and leaves it
``Configuring``; ``start_session()`` freezes the session; ``step(n)`` runs ticks on the
calling thread with the GIL released; ``close()`` waits for running calls under the
shutdown deadline and releases everything, and never raises because shutdown failed.
Logging is acquired around the native construction and released after the native
release, so the diagnostics of a failed construction are never lost.

The thin native mirror is ``sim_estab._if.engine``; this module adds what is Pythonic:
the context manager, the owner-thread check, keyword registration, the Python-style
error names and the ``on_failed_stop`` policy. Only the owner thread, the one that
constructed the engine, may make lifecycle and pump calls; the four control calls may
come from any thread (``design_python_api.md`` §6).
"""

from __future__ import annotations

import logging
import os
import threading
import warnings
from types import TracebackType
from typing import Callable, Final, Iterable, Mapping, TypeVar

from . import _if
from .config import EngineConfig
from .upkeep import log as upkeep_log

__all__ = [
    "Block",
    "Engine",
    "EngineClosedError",
    "EngineError",
    "EngineExistsError",
    "EngineFailedError",
    "EngineState",
    "EngineStateError",
    "EngineThreadError",
    "EngineView",
    "EventBacklogError",
    "SimEstabError",
    "SinkDigest",
    "StepEnd",
    "StepResult",
    "Tick",
]

_logger = logging.getLogger("sim_estab.engine")

# The bound types, under the names design_python_api.md uses. Each is the same object
# as its _if counterpart, so `except` works with either spelling.
SimEstabError = _if.error.sim_estab_error
EngineError = _if.engine.engine_error
EngineStateError = _if.engine.engine_state_error
EngineClosedError = _if.engine.engine_closed_error
EngineFailedError = _if.engine.engine_failed_error
EngineExistsError = _if.engine.engine_exists_error
EventBacklogError = _if.engine.event_backlog_error

EngineState = _if.engine.EngineState
StepEnd = _if.engine.StepEnd
StepResult = _if.engine.StepResult
SinkDigest = _if.engine.SinkDigest
Tick = _if.engine.Tick
Block = _if.engine.Block


class EngineThreadError(EngineError):
    """A lifecycle or pump call off the owner thread (``design_python_api.md`` §2, §6).
    Nothing was touched: make the call from the owner thread."""


_EVENT_DRAINS: Final[Mapping[str, _if.engine.EventDrain]] = {
    "owner": _if.engine.EventDrain.Owner,
    "sink": _if.engine.EventDrain.Sink,
}

_PREDICATES: Final[Mapping[str, _if.engine.RowPredicateType]] = {
    "all": _if.engine.RowPredicateType.All,
}

_ON_EXPIRY: Final[Mapping[str, _if.engine.OnExpiry]] = {
    "fail": _if.engine.OnExpiry.Fail,
    "continue_without": _if.engine.OnExpiry.ContinueWithout,
}

_T = TypeVar("_T")


def _native_config(config: EngineConfig) -> _if.engine.EngineConfig:
    """The one conversion of the config at the boundary (``design_python_api.md`` §3).

    ``entity_capacity``, ``projection_warn_bytes_per_second`` and the ``HostPolicy``
    fields other than ``shutdown_deadline`` have no native mirror yet: they arrive with
    the world container (M3), projection (M5) and mod hosts (M7).
    """
    native = _if.engine.EngineConfig()
    native.tick_rate = config.tick_rate
    native.seed = config.seed
    native.event_drain = (
        None if config.event_drain is None else _EVENT_DRAINS[config.event_drain]
    )
    native.drain_interval_ticks = config.command_policy.drain_interval_ticks
    native.endpoint_capacity = config.command_policy.endpoint_capacity
    native.host_endpoint_capacity = config.command_policy.host_endpoint_capacity
    native.peer_endpoint_capacity = config.command_policy.peer_endpoint_capacity
    native.shutdown_deadline = float(config.host_policy.shutdown_deadline)
    return native


class EngineView:
    """The reader's handle on an engine view (``design_python_api.md`` §7.2), from
    ``Engine.view()``.

    It belongs to one reader thread, whichever thread that is. ``take()`` exchanges the
    held block for the newest published one; on a paced engine view that take is the
    reader's declaration. After ``close()`` every call raises ``EngineClosedError``.
    """

    __slots__ = ("_handle",)

    def __init__(self, handle: _if.engine.EngineViewHandle) -> None:
        self._handle = handle

    def take(self) -> bool:
        """Take the newest published block. ``True`` if a newer block was taken;
        ``False`` keeps the held one and declares nothing."""
        return self._handle.take()

    def held(self) -> Block:
        """A copy of the block held: the tick it shows and its payload. It shows a
        snapshot once the first successful ``take()``."""
        return self._handle.held()

    @property
    def name(self) -> str:
        return self._handle.name()

    @property
    def cadence(self) -> int:
        return self._handle.cadence()

    @property
    def paced(self) -> bool:
        return self._handle.is_paced()

    def __repr__(self) -> str:
        return f"EngineView({self.name!r}, cadence={self.cadence}, paced={self.paced})"


class Engine:
    """The engine (``design_python_api.md`` §2): one per process, a context manager.

    ``with Engine(config) as engine:`` acquires; ``__exit__`` stops the session and
    closes, unconditionally. Without ``with``, call ``close()`` yourself: relying on the
    finalizer is unsupported, and it warns.
    """

    def __init__(self, config: EngineConfig) -> None:
        if not isinstance(config, EngineConfig):
            raise TypeError(f"config must be an EngineConfig, got {type(config)!r}")
        if not config.headless:
            raise NotImplementedError(
                "a windowed session arrives with the window (M6); construct with "
                "headless=True"
            )
        self._config = config
        self._owner = threading.get_ident()
        self._native: _if.engine.Engine | None = None
        self._closed = False

        # Logging is acquired around the native construction, not inside it
        # (design_python_api.md §2), and released once, on the first close().
        upkeep_log.acquire(config.log)
        try:
            self._native = _if.engine.Engine(_native_config(config))
        except BaseException:
            upkeep_log.release()
            raise

    # ------------------------------------------------------------------
    # the context manager and the finalizer
    # ------------------------------------------------------------------

    def __enter__(self) -> Engine:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        # The shutdown chain of design_python_api.md §2: stop ticking, then close.
        # close() is unconditional, and a stop-time error never masks the body's
        # exception.
        try:
            if exc_type is None:
                self.stop_sim_async()
            else:
                try:
                    self.stop_sim_async()
                except Exception:
                    _logger.exception(
                        "stop_sim_async() failed while the with body was unwinding; "
                        "the body's exception is raised instead"
                    )
        finally:
            self.close()

    def __del__(self) -> None:
        native = getattr(self, "_native", None)
        if native is None or getattr(self, "_closed", True):
            return
        warnings.warn(
            "Engine was never closed: closing from the finalizer; call close() or use "
            "`with` (design_python_api.md §2)",
            ResourceWarning,
            source=self,
        )
        # The native destructor closes. Logging is released after it, as close() would.
        self._native = None
        self._closed = True
        upkeep_log.release()

    def __repr__(self) -> str:
        return f"Engine(state={self.state.name})"

    # ------------------------------------------------------------------
    # the checks every call goes through
    # ------------------------------------------------------------------

    def _owner_only(self, call: str) -> None:
        if threading.get_ident() != self._owner:
            raise EngineThreadError(
                f"{call} must be made on the owner thread, the thread that "
                f"constructed the Engine (design_python_api.md §6); nothing was touched"
            )

    def _native_engine(self) -> _if.engine.Engine:
        assert self._native is not None  # cleared only by the finalizer
        return self._native

    def _on_failed(self, call: str) -> None:
        """A `Failed` entry path, under ``on_failed_stop`` (``design_python_api.md``
        §3): ``"raise"`` lets the error propagate, ``"terminate"`` aborts the process.
        No process host and no shared-memory object exists in M1, so the cleanup
        before the abort has nothing to do."""
        if self._config.on_failed_stop == "terminate":
            _logger.critical(
                "%s: the engine is Failed and on_failed_stop is 'terminate'; "
                "aborting the process",
                call,
            )
            os.abort()

    def _failed_path(self, call: str, fn: Callable[..., _T], *args: object) -> _T:
        """Run a native call that can enter `Failed`, applying ``on_failed_stop``."""
        try:
            return fn(*args)
        except EngineFailedError:
            self._on_failed(call)
            raise

    # ------------------------------------------------------------------
    # queries
    # ------------------------------------------------------------------

    @property
    def state(self) -> EngineState:
        """Any thread: the engine state."""
        return self._native_engine().state()

    @property
    def config(self) -> EngineConfig:
        """The frozen config the engine was constructed with."""
        return self._config

    def seed(self) -> int:
        """Running or Stopped: the seed the freeze resolved, from the config or
        generated."""
        self._owner_only("seed()")
        return self._native_engine().seed()

    # ------------------------------------------------------------------
    # Configuring
    # ------------------------------------------------------------------

    def register_engine_view(
        self,
        name: str,
        *,
        max_payload_bytes: int = 0,
        identity: bool = True,
        predicate: str = "all",
        cadence: int = 1,
        paced: bool = False,
        deadline_ms: int = 0,
        on_expiry: str = "fail",
    ) -> None:
        """Configuring: declare an engine view (``design_python_api.md`` §7.2).

        ``predicate`` names the row predicate type (only ``"all"`` until projection,
        M5). A ``paced`` engine view makes its reader a participant: the gate waits up
        to ``deadline_ms`` for each take, and ``on_expiry`` (``"fail"`` or
        ``"continue_without"``) says what a passed deadline does.
        """
        self._owner_only("register_engine_view()")
        if predicate not in _PREDICATES:
            raise ValueError(
                f"predicate must be one of {', '.join(_PREDICATES)}, got {predicate!r}"
            )
        if on_expiry not in _ON_EXPIRY:
            raise ValueError(
                f"on_expiry must be one of {', '.join(_ON_EXPIRY)}, got {on_expiry!r}"
            )
        if cadence < 1:
            raise ValueError(f"cadence must be at least 1, got {cadence}")
        if max_payload_bytes < 0 or deadline_ms < 0:
            raise ValueError("max_payload_bytes and deadline_ms must not be negative")
        spec = _if.engine.EngineViewSpec()
        spec.name = name
        spec.max_payload_bytes = max_payload_bytes
        spec.identity = identity
        spec.predicate = _PREDICATES[predicate]
        spec.cadence = cadence
        spec.paced = paced
        spec.deadline_ms = deadline_ms
        spec.on_expiry = _ON_EXPIRY[on_expiry]
        self._native_engine().register_engine_view(spec)

    def start_session(self) -> None:
        """Configuring: the freeze (``design_engine_core.md`` §2.4). Always explicit,
        always the caller's. Raises ``ValueError`` while the config declares no event
        drain, before anything is frozen."""
        self._owner_only("start_session()")
        if self._config.event_drain is None:
            raise ValueError(
                "EngineConfig.event_drain is None: a session declares who empties its "
                "event ring, 'owner' or 'sink' (design_python_api.md §3)"
            )
        self._native_engine().start_session()

    # ------------------------------------------------------------------
    # Running: the pump
    # ------------------------------------------------------------------

    def step(self, tick_count: int) -> StepResult:
        """Running: run ``tick_count`` ticks on this thread, with the GIL released
        (``design_python_api.md`` §4.3). Returns the tick reached and what ended the
        step; a ``KeyboardInterrupt`` ends it at a tick boundary. Raises
        ``EventBacklogError`` at the backlog mark under an owner drain: drain, then
        step again."""
        self._owner_only("step()")
        return self._failed_path("step()", self._native_engine().step, tick_count)

    def submit(self, command: _if.engine.Command):
        """Running or Stopped: submit a command on the host endpoint. Never waits:
        returns the ``CommandHandle``, or the ``Rejection`` (``design_python_api.md``
        §7.1)."""
        self._owner_only("submit()")
        return self._native_engine().submit(command)

    def submit_batch(self, commands: Iterable[_if.engine.Command]) -> list:
        """Running or Stopped: submit commands in order, in one crossing. One
        ``CommandHandle`` or ``Rejection`` per command, in array order."""
        self._owner_only("submit_batch()")
        return self._native_engine().submit_batch(list(commands))

    def outcome(self, handle: _if.engine.CommandHandle) -> _if.engine.CommandOutcome:
        """Running or Stopped: a host command's outcome. Never waits: ``Pending``
        until the tick that drained it has run."""
        self._owner_only("outcome()")
        return self._native_engine().outcome(handle)

    def outcomes(
        self, handles: Iterable[_if.engine.CommandHandle]
    ) -> list[_if.engine.CommandOutcome]:
        """The outcomes of ``handles``, in order."""
        self._owner_only("outcomes()")
        native = self._native_engine()
        return [native.outcome(handle) for handle in handles]

    def view(self, name: str) -> EngineView:
        """Running or Stopped: the handle of the engine view registered as ``name``.
        The one stated exception to never shortening "engine view": ``engine.view``."""
        self._owner_only("view()")
        return EngineView(self._native_engine().view(name))

    def drain_events(self) -> list[_if.engine.EngineEvent]:
        """Running or Stopped, owner drain: every event raised so far, in tick order.
        Never blocks; *not* calling it is what pauses the sim
        (``design_python_api.md`` §7.3)."""
        self._owner_only("drain_events()")
        return self._native_engine().drain_events()

    def sink_digest(self) -> SinkDigest:
        """Running or Stopped, sink drain: what the sink has consumed."""
        self._owner_only("sink_digest()")
        return self._native_engine().sink_digest()

    def snapshot(self) -> Tick:
        """Running or Stopped, while no step is in flight: the last tick that has run.
        The M1 snapshot is the tick alone (``design_engine_core.md`` §7 step 1)."""
        self._owner_only("snapshot()")
        return self._native_engine().snapshot()

    # ------------------------------------------------------------------
    # the control calls: any thread
    # ------------------------------------------------------------------

    def pause(self) -> None:
        """Any thread: no tick runs after the one in progress, until a resume or a
        step."""
        self._native_engine().pause()

    def resume(self) -> None:
        """Any thread: lift the ceiling on the run grant. Raises ``EngineStateError``
        while a step is in flight."""
        self._native_engine().resume()

    def request_stop(self) -> None:
        """Any thread: no tick runs again in this session. Sticky."""
        self._native_engine().request_stop()

    def stop_requested(self) -> bool:
        """Any thread: whether a stop was requested."""
        return self._native_engine().stop_requested()

    # ------------------------------------------------------------------
    # shutdown
    # ------------------------------------------------------------------

    def stop_sim_async(self) -> None:
        """Running: end ticking for the session; the engine is ``Stopped``. With no
        sim thread there is nothing to join. A no-op in every other state but
        ``Failed``, which raises."""
        self._owner_only("stop_sim_async()")
        self._failed_path("stop_sim_async()", self._native_engine().stop_sim_async)

    def close(self) -> None:
        """Owner thread: shut down (``design_python_api.md`` §2). Stops ticking if the
        caller has not, closes the operation lease, waits for running calls under the
        shutdown deadline, releases the session, then releases logging. Idempotent, and
        it never raises because shutdown failed: a wait that expires leaves the engine
        ``Failed`` and is logged, or aborts under ``on_failed_stop="terminate"``."""
        self._owner_only("close()")
        if self._closed:
            return
        native = self._native_engine()
        failed_before = native.state() == EngineState.Failed
        native.close()
        self._closed = True
        if not failed_before and native.state() == EngineState.Failed:
            self._on_failed("close()")
        upkeep_log.release()
