"""The logging subsystem's Python side: the refcounted holder over the C++ triad, and
the 1:1 functions (``design_logging.md`` §2, §3).

Logging starts before every other subsystem and stops after the last. ``acquire()``
initializes it for the first holder and ``release()`` shuts it down for the last; the
``Engine`` brackets its native construction and release with the two, and an
application that acquired logging first keeps its sinks. The count lives under a
module-level lock, so both calls are one step from any thread, on the GIL build and the
free-threaded build alike.
"""

from __future__ import annotations

import logging
import threading
import warnings
from logging import LogRecord
from typing import Optional

from .. import _if
from ..config import LOG_LEVELS, LogConfig

# module local logger
_logger = logging.getLogger(__name__)

# root logger
_root_logger = logging.getLogger()
_engine_handler: Optional[logging.Handler] = None
# The root level log_init found, restored by log_deinit (design_logging.md §2)
_saved_root_level: int = logging.NOTSET

# The refcount (design_logging.md §2): the first holder initializes, the last tears
# down, and the first holder's config wins. The lock makes each of those one step.
_lock = threading.Lock()
_holders = 0
_held_config: Optional[LogConfig] = None


class EngineLogHandler(logging.Handler):
    """Python logging handler that forwards to SimEstab C++ engine logging."""

    def __init__(self, level: int = logging.NOTSET):
        super().__init__(level)

    def emit(self, record: LogRecord):
        """Forward Python log records to C++ engine logging."""
        # Convert Python logging level to C++ severity
        severity_map = {
            logging.DEBUG: _if.log.severity_level.debug,
            logging.INFO: _if.log.severity_level.info,
            logging.WARNING: _if.log.severity_level.warning,
            logging.ERROR: _if.log.severity_level.error,
            logging.CRITICAL: _if.log.severity_level.critical,
            logging.FATAL: _if.log.severity_level.critical,  # Map FATAL to CRITICAL
        }

        # Find closest severity level (find the first level that matches or exceeds)
        level = min(
            [py_log for py_log in severity_map.keys() if py_log >= record.levelno],
            default=logging.INFO,
        )
        severity = severity_map[level]

        # Use logger name as C++ channel for hierarchical logging
        channel = record.name

        # Format the message using the handler's formatter
        message = self.format(record)

        # Forward to C++ engine logging
        try:
            _if.log.log(channel, severity, message)
        except Exception:
            self.handleError(record)


def log_init():
    """Initialize the logging system and add Python engine handler."""
    global _engine_handler, _saved_root_level

    _if.log.log_init()

    # Add engine handler that forwards Python logs to C++ engine. The bridge needs every
    # record, so the root level goes to DEBUG; log_deinit restores the saved one.
    if _engine_handler is None:
        _saved_root_level = _root_logger.level
        _root_logger.setLevel(logging.DEBUG)
        _engine_handler = EngineLogHandler()
        _engine_handler.setFormatter(logging.Formatter("%(message)s"))
        _root_logger.addHandler(_engine_handler)


def log_deinit():
    """Deinitialize the logging system and remove Python engine handler."""
    global _engine_handler

    # Remove engine handler if it exists, and restore the root level log_init found. A
    # level the application set while logging was held stays, with a warning.
    if _engine_handler is not None:
        _root_logger.removeHandler(_engine_handler)
        _engine_handler = None
        level = _root_logger.level
        if level == logging.DEBUG:
            _root_logger.setLevel(_saved_root_level)
        elif level != _saved_root_level:
            warnings.warn(
                f"the root logger level is {logging.getLevelName(level)}, set while "
                f"logging was held; it stays, and log_deinit does not restore "
                f"{logging.getLevelName(_saved_root_level)} (design_logging.md §2)",
                RuntimeWarning,
                stacklevel=2,
            )

    _if.log.log_deinit()


def enable_console():
    _if.log.enable_console()


def disable_console():
    _if.log.disable_console()


def _severity(name: str, field: str) -> _if.log.severity_level:
    if name not in LOG_LEVELS:
        raise ValueError(
            f"{field} must be one of {', '.join(LOG_LEVELS)}, got {name!r}"
        )
    return getattr(_if.log.severity_level, name)


def set_severity(level: str) -> None:
    """The user's level, one of ``LOG_LEVELS`` (``design_logging.md`` §3): the console's
    filter and, with the open files' floors, the core filter. A file's band is fixed at
    its open, so this never changes a file."""
    _if.log.set_severity(_severity(level, "level"))


def acquire(config: LogConfig | None = None) -> None:
    """Hold the logging subsystem: initialize it for the first holder, count for the
    rest.

    The first holder's ``config`` is applied and kept: the level and the console, in that
    order. A later holder with a different one is warned about and ignored
    (``design_logging.md`` §2: first caller wins). ``session_dir`` is accepted but not
    applied yet: the session file is planned (``design_logging.md`` §4), and a value is
    warned about until it exists.
    """
    global _holders, _held_config

    if config is None:
        config = LogConfig()
    with _lock:
        if _holders == 0:
            log_init()
            try:
                set_severity(config.level)
                if config.console:
                    enable_console()
                else:
                    disable_console()
            except BaseException:
                log_deinit()
                raise
            _held_config = config
            if config.session_dir is not None:
                warnings.warn(
                    f"LogConfig.session_dir={str(config.session_dir)!r} is not applied "
                    f"yet: the session file is planned (design_logging.md §4)",
                    RuntimeWarning,
                    stacklevel=2,
                )
        elif config != _held_config:
            warnings.warn(
                f"logging is already held with {_held_config!r}; the different "
                f"{config!r} is ignored: the first holder's configuration wins "
                f"(design_logging.md §2)",
                RuntimeWarning,
                stacklevel=2,
            )
        _holders += 1


def release() -> None:
    """Let go of the logging subsystem: shut it down for the last holder, count for the
    rest. Raises ``RuntimeError`` without a matching ``acquire()``."""
    global _holders, _held_config

    with _lock:
        if _holders == 0:
            raise RuntimeError("release() without a matching acquire()")
        _holders -= 1
        if _holders == 0:
            _held_config = None
            log_deinit()


def holder_count() -> int:
    """How many holders have acquired logging and not released it."""
    with _lock:
        return _holders


# TODO
def enable_file(filename):
    pass


# TODO
def disable_file(filename):
    pass


# interactive fn
def ask_for_confirm():
    global _console_handler
    if _console_handler is not None:
        _logger.warning("ask for confirmation!")
        while True:
            ret = input("confirm (y/n)?")
            if ret.upper() == "Y":
                _logger.info("-> confirm")
                return True
            elif ret.upper() == "N":
                _logger.info("-> exit")
                return False
    else:
        return True
