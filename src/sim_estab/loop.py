"""The reference loops (``design_python_api.md`` §4).

M1 runs headless: ``run_headless`` steps the engine on the calling thread. The windowed
loop arrives with the window and the sim thread (M6), and the replay loop with replay
(M2).
"""

from __future__ import annotations

from typing import Callable, Sequence

from .engine import Engine, EventBacklogError, StepEnd, Tick
from .event import EngineEvent

__all__ = ["run_headless"]


def run_headless(
    engine: Engine,
    ticks: int,
    *,
    on_events: Callable[[Sequence[EngineEvent]], None] | None = None,
) -> Tick:
    """Run ``ticks`` ticks on the calling thread (``design_python_api.md`` §4.2), and
    return the tick reached.

    Under an owner drain the step raises ``EventBacklogError`` at the backlog mark;
    the loop drains, hands the batch to ``on_events`` and steps on, and it drains once
    more when it ends, so no event waits in the ring. Under a sink drain the engine
    empties the ring itself and ``on_events`` is never called. A stop, or a
    ``KeyboardInterrupt``, ends the loop early with the tick actually reached.
    """
    if ticks < 0:
        raise ValueError(f"ticks must not be negative, got {ticks}")
    owner_drain = engine.config.event_drain == "owner"

    def drain() -> None:
        events = engine.drain_events()
        if on_events is not None:
            on_events(events)

    reached = engine.snapshot()
    target = int(reached) + ticks
    while int(reached) < target:
        try:
            result = engine.step(target - int(reached))
        except EventBacklogError as backlog:
            reached = backlog.tick_reached
            drain()
            continue
        reached = result.tick_reached
        if result.end != StepEnd.GrantEnded:
            break
    if owner_drain:
        drain()
    return reached
