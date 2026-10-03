"""Engine configuration: frozen option objects, validated before the boundary.

Every value here is decided in ``doc/design_limits.md``, which carries the reasoning
and the revisit trigger for each one; the shapes are ``design_python_api.md`` §3. The
C++ mirror of the command-policy defaults, the tick rate, the warn threshold and the
entity capacities is the ``sim_estab:limits`` module partition, and the two must not
drift. ``HostPolicy`` values
have no C++ mirror.

Validation happens here, in Python, before anything touches native code
(``design_patterns.md`` §6: validate arguments up front). ``ValueError`` carries a
specific message naming the field and the constraint it broke.

Not yet present, and deliberately: ``EngineConfig.window`` and ``EngineConfig.log``.
Both are M1 surface and neither carries a number M0 owns, so inventing them here would
pre-decide shapes that belong with the session lifecycle.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Final, Mapping

__all__ = [
    "CommandPolicy",
    "EngineConfig",
    "HostPolicy",
]


#: Ceiling a mod manifest may request for its per-endpoint, per-tick capacity.
MAX_SOURCE_CAPACITY: Final[int] = 256

#: The cap of every object type that declares none: the most entities of that type
#: alive at one time (``doc/design_limits.md`` §4). A ceiling, not a working size.
DEFAULT_ENTITY_CAPACITY: Final[int] = 1 << 24

#: The largest cap a type may declare or a session may configure. A slot is a ``u32``
#: and the high half of a ``u64`` id, so a larger cap has no slot to put an entity in.
MAX_ENTITY_CAPACITY: Final[int] = (1 << 32) - 1


def _require_positive(value: int | float, name: str) -> None:
    if value <= 0:
        raise ValueError(f"{name} must be positive, got {value!r}")


@dataclass(frozen=True, slots=True)
class CommandPolicy:
    """Per-endpoint capacity and the drain interval for the inbound command path
    (``design_python_api.md`` §7.1).

    **Capacity is the only number an endpoint declares.** There is no drain quota: a
    tick executes everything stamped for it, because pacing closes the set before the
    drain runs (``design_engine_core.md`` §5.1).

    Deliberately **not** here, and not as properties either: ring ``DEPTH``
    (``capacity``, or ``2 × capacity`` for an endpoint with a stamp margin), ``C`` (the
    engine-wide commands executed per tick, ``Σ capacity(endpoint)``) and the event ring
    size ``E × (D + 1)``. The sets they follow are not closed until the freeze, so none
    of the three is knowable at construction. Exposing any of them would create an
    engine-wide pool, which per-endpoint rings avoid, and would be wrong for every
    session that loads a different number of mods (``doc/design_limits.md`` §2, §8).
    """

    #: ``D`` — ticks between two ``drain_events()`` calls that the event ring is sized
    #: to buffer. A host exceeding it is a host defect: the event backlog pauses the
    #: simulation and is reported, never silently dropped
    #: (``design_engine_core.md`` §5.2).
    drain_interval_ticks: int = 8

    #: Commands ONE endpoint may hold for ONE tick, when a manifest declares none.
    #: Exceeding it is ``queue_full`` at the submitting call, and it is the only
    #: capacity rejection in the engine.
    source_capacity: int = 64

    #: The host endpoint (source 0) gets the ceiling. It needs no reserve carved out of
    #: an engine-wide pool, because rings are per endpoint: no mod can consume the host's
    #: capacity, whatever it submits.
    host_source_capacity: int = 256

    #: A peer endpoint carries a whole remote player's turn, so it gets the ceiling for
    #: the same reason the host does.
    peer_source_capacity: int = 256

    def __post_init__(self) -> None:
        _require_positive(self.drain_interval_ticks, "drain_interval_ticks")

        for name, capacity in (
            ("source_capacity", self.source_capacity),
            ("host_source_capacity", self.host_source_capacity),
            ("peer_source_capacity", self.peer_source_capacity),
        ):
            _require_positive(capacity, name)
            if capacity > MAX_SOURCE_CAPACITY:
                raise ValueError(
                    f"{name} must not exceed the engine cap of "
                    f"{MAX_SOURCE_CAPACITY}, got {capacity}"
                )


@dataclass(frozen=True, slots=True)
class HostPolicy:
    """What the *engine* enforces on mod hosts (``design_python_api.md`` §3).

    Distinct from ``ModPolicy``, which decides which mods exist and what they may do
    and is an argument to ``load_mods`` rather than a config field. Everything here is
    needed by ``close()`` whether or not a mod was ever loaded.
    """

    #: Seconds, reused independently by *every* blocking shutdown stage: waiting for
    #: endpoint submits, mod host inbox drain + ``on_unload``, mod host and sim thread
    #: joins, confirm-death after a process kill, and waiting for running engine calls. One value keeps the escalation policy
    #: coherent; it does **not** mean one aggregate deadline across the five.
    shutdown_deadline: float = 5.0

    #: Seconds bounding a per-call IPC round trip before it becomes ``host_error``, or
    #: ``None`` to leave it unbounded.
    #:
    #: **Deliberately undecided** (``doc/design_limits.md`` §1.1, §7). It is not a free
    #: parameter: its ceiling *is* the input delay — the window in which a peer's
    #: command may legitimately be in flight — so it lands with ``input_delay_ticks``
    #: and not before, against a measured RTT (``doc/open_question.md`` Q43).
    #:
    #: A value, when given, **must exceed one tick**. A deadline shorter than that can
    #: only ever detect "the other side is mid-tick", which is not a fault, and would
    #: expire on any hot-path round trip spanning a tick boundary. ``EngineConfig``
    #: enforces it, because the tick rate lives there and not here. It bounds the
    #: hot-path round trip only; a process mod's *outcome* wait is a separate,
    #: cancellable operation with no deadline of this kind.
    ipc_deadline: float | None = None

    #: Quarantine: 0 keeps a failed mod disabled, N re-spawns up to N times. One
    #: number, not a mode plus a number.
    mod_retry_limit: int = 0

    #: Engine ceiling on the ``inbox_size`` a manifest requests. The cap exists because
    #: each mod declares its own capacity, so without one total inbox memory scales
    #: with how many mods the user installed rather than with anything the engine chose.
    max_inbox_size: int = 1024

    def __post_init__(self) -> None:
        _require_positive(self.shutdown_deadline, "shutdown_deadline")
        _require_positive(self.max_inbox_size, "max_inbox_size")

        if self.ipc_deadline is not None:
            _require_positive(self.ipc_deadline, "ipc_deadline")

        if self.mod_retry_limit < 0:
            raise ValueError(
                f"mod_retry_limit must not be negative, got {self.mod_retry_limit}"
            )


@dataclass(frozen=True, slots=True)
class EngineConfig:
    """The one config object, immutable after construction.

    Converted once at the boundary; ``Engine(config)`` is the only place it crosses.
    """

    headless: bool = False

    #: Fixed simulation Hz — the tick counter *is* time. Not a speed knob: changing it
    #: changes what one tick means and therefore changes outcomes, which is why it is
    #: frozen at construction and sits in the replay header. Game speed is
    #: ``set_time_scale``, a control call, outside determinism.
    tick_rate: int = 30

    host_policy: HostPolicy = field(default_factory=HostPolicy)
    command_policy: CommandPolicy = field(default_factory=CommandPolicy)

    #: Cap overrides, per object type: the most entities of that type alive at one
    #: time. Empty means every type keeps its declared ``[[=cap(N)]]``, or
    #: ``DEFAULT_ENTITY_CAPACITY``. Each entry must be 1 to ``MAX_ENTITY_CAPACITY``.
    #: Every cap is closed at the freeze and joins session identity, and a create at the
    #: cap is refused deterministically (``doc/design_data_container.md`` §2.2).
    entity_capacity: Mapping[str, int] = field(default_factory=dict)

    #: ``None`` generates one, which is then recorded.
    seed: int | None = None

    #: Join-timeout policy: ``"raise"`` or ``"terminate"``.
    on_failed_stop: str = "raise"

    #: Publish bandwidth above which the engine emits a warning naming the measured
    #: rate **and the engine view**. Evaluated **per engine view**, never against the
    #: session total: every remedy is a change to one declaration — a narrower spec, a
    #: tighter row predicate, a lower cadence — so a crossing has to name one. The
    #: projection is deliberately uncapped (``doc/design_limits.md`` §5); its cost is
    #: measured instead.
    #:
    #: 4 GB/s is reasoned, not measured. One unfiltered engine view over 10⁶ live rows
    #: at a 32-byte spec is 0.96 GB/s at 30 Hz, the scale this design budgets for, so a
    #: threshold near 1 GB/s would warn during ordinary operation.
    projection_warn_bytes_per_second: int = 4_000_000_000

    def __post_init__(self) -> None:
        _require_positive(self.tick_rate, "tick_rate")
        _require_positive(
            self.projection_warn_bytes_per_second, "projection_warn_bytes_per_second"
        )

        if self.on_failed_stop not in ("raise", "terminate"):
            raise ValueError(
                f"on_failed_stop must be 'raise' or 'terminate', got "
                f"{self.on_failed_stop!r}"
            )
        if self.seed is not None and self.seed < 0:
            raise ValueError(f"seed must not be negative, got {self.seed}")

        # `ipc_deadline` is checked here rather than on HostPolicy because the constraint
        # is stated against the tick rate, which HostPolicy cannot see.
        ipc_deadline = self.host_policy.ipc_deadline
        if ipc_deadline is not None:
            one_tick = 1.0 / self.tick_rate
            if ipc_deadline <= one_tick:
                raise ValueError(
                    f"ipc_deadline must exceed one tick ({one_tick:.4f}s at "
                    f"tick_rate={self.tick_rate}), got {ipc_deadline}; a shorter "
                    f"deadline detects only that the other side is mid-tick, which is "
                    f"not a fault"
                )

        for name, cap in self.entity_capacity.items():
            if not 1 <= cap <= MAX_ENTITY_CAPACITY:
                raise ValueError(
                    f"entity_capacity[{name!r}] must be 1 to {MAX_ENTITY_CAPACITY}, got "
                    f"{cap}; a slot is a u32, and omitting the entry keeps the type's "
                    f"declared or default cap"
                )

        # Immutable after construction, in fact and not only by convention: a frozen
        # dataclass would otherwise still hand out a mutable mapping.
        object.__setattr__(
            self, "entity_capacity", MappingProxyType(dict(self.entity_capacity))
        )
