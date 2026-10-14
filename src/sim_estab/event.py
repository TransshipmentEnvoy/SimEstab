"""Events: what happened in the engine core (``design_python_api.md`` §7.3).

Events travel on the event ring to the session's event drain: the owner thread, which
empties the ring with ``Engine.drain_events()`` between ticks, or the engine's own sink,
which counts them and keeps nothing. An event is tick-stamped and typed; ``topic()``
gives the dotted name the design docs use for each type.
"""

from __future__ import annotations

from types import MappingProxyType
from typing import Final, Mapping

from . import _if

__all__ = [
    "EngineEvent",
    "EngineEventType",
    "EventDrain",
    "TOPICS",
    "topic",
]

EngineEvent = _if.engine.EngineEvent
EngineEventType = _if.engine.EngineEventType
EventDrain = _if.engine.EventDrain

#: The topic of each event type, as ``design_python_api.md`` §7.3 names it.
TOPICS: Final[Mapping[EngineEventType, str]] = MappingProxyType(
    {
        EngineEventType.SessionStarted: "session.started",
        EngineEventType.SessionStopped: "session.stopped",
        EngineEventType.SimBacklogPaused: "sim.backlog_paused",
        EngineEventType.SimBacklogResumed: "sim.backlog_resumed",
        EngineEventType.ParticipantExpired: "participant.expired",
        EngineEventType.CommandProtocolError: "command.protocol_error",
    }
)


def topic(event_type: EngineEventType) -> str:
    """The dotted topic of ``event_type``, such as ``"session.started"``."""
    return TOPICS[event_type]
