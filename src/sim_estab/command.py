"""Commands: the inbound write path (``design_python_api.md`` §7.1).

Every command gets two answers. ``Engine.submit`` answers *is it in the queue?* with a
``CommandHandle`` or a ``Rejection``, returned and never raised. ``Engine.outcome``
answers *what happened when it ran?*: ``Pending`` until the tick that drained it has
run, then ``Applied`` or ``Dropped`` with that tick, and ``Expired`` once the producer
has submitted a ring depth of commands after it.

M1 carries no simulation content, so a command is its tick and an opaque payload, and
the one command, ``noop()``, does nothing when it runs (``design_engine_core.md`` §7
step 1). The typed builders arrive with the command payload schema (M2).
"""

from __future__ import annotations

from typing import Final

from . import _if

__all__ = [
    "Command",
    "CommandHandle",
    "CommandOutcome",
    "CommandOutcomeType",
    "MAX_PAYLOAD_BYTES",
    "Rejection",
    "noop",
]

Command = _if.engine.Command
CommandHandle = _if.engine.CommandHandle
CommandOutcome = _if.engine.CommandOutcome
CommandOutcomeType = _if.engine.CommandOutcomeType
Rejection = _if.engine.Rejection

#: The bytes a command payload holds. Provisional: the slot size waits for the payload
#: schema (``doc/open_question.md`` Q22).
MAX_PAYLOAD_BYTES: Final[int] = _if.engine.provisional_command_payload_bytes


def noop(payload: bytes = b"") -> Command:
    """The M1 command: it does nothing when it runs, and its outcome is ``Applied`` with
    the tick that ran it. ``payload`` rides along opaque, zero-padded to
    ``MAX_PAYLOAD_BYTES``; the engine never reads it."""
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise ValueError(
            f"a command payload holds at most {MAX_PAYLOAD_BYTES} bytes, got "
            f"{len(payload)}"
        )
    command = Command()
    command.payload = bytes(payload)
    return command
