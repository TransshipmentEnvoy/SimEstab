import os
import platform


def prepare_dll():
    this_dir = os.path.normpath(__file__)
    root_dir = os.path.dirname(this_dir)
    lib_dir = os.path.join(root_dir, "lib")
    os.add_dll_directory(lib_dir)


if platform.system() == "Windows":
    prepare_dll()

from . import _if  # noqa: E402,F401  (loads the extension)
from .command import (  # noqa: E402
    Command,
    CommandHandle,
    CommandOutcome,
    CommandOutcomeType,
    Rejection,
    noop,
)
from .config import (  # noqa: E402
    CommandPolicy,
    EngineConfig,
    HostPolicy,
    LogConfig,
    WindowConfig,
)
from .engine import (  # noqa: E402
    Block,
    Engine,
    EngineClosedError,
    EngineError,
    EngineExistsError,
    EngineFailedError,
    EngineState,
    EngineStateError,
    EngineThreadError,
    EngineView,
    EventBacklogError,
    SimEstabError,
    SinkDigest,
    StepEnd,
    StepResult,
    Tick,
)
from .event import EngineEvent, EngineEventType, EventDrain, topic  # noqa: E402
from .loop import run_headless  # noqa: E402

__all__ = [
    "Block",
    "Command",
    "CommandHandle",
    "CommandOutcome",
    "CommandOutcomeType",
    "CommandPolicy",
    "Engine",
    "EngineClosedError",
    "EngineConfig",
    "EngineError",
    "EngineEvent",
    "EngineEventType",
    "EngineExistsError",
    "EngineFailedError",
    "EngineState",
    "EngineStateError",
    "EngineThreadError",
    "EngineView",
    "EventBacklogError",
    "EventDrain",
    "HostPolicy",
    "LogConfig",
    "Rejection",
    "SimEstabError",
    "SinkDigest",
    "StepEnd",
    "StepResult",
    "Tick",
    "WindowConfig",
    "noop",
    "run_headless",
    "topic",
]
