from .. import _if

import logging
from logging import LogRecord
import logging.handlers
import os
import multiprocessing as mp  # ordinary mp
from copy import deepcopy

from typing import Any, Optional

# module local logger
_logger = logging.getLogger(__name__)

# root logger
_root_logger = logging.getLogger()
_engine_handler: Optional[logging.Handler] = None


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
        level = min([py_log for py_log in severity_map.keys() if py_log >= record.levelno],
                   default=logging.INFO)
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
    global _engine_handler

    _if.log.log_init()
    _root_logger.setLevel(logging.DEBUG)

    # Add engine handler that forwards Python logs to C++ engine
    if _engine_handler is None:
        _engine_handler = EngineLogHandler()
        _engine_handler.setFormatter(logging.Formatter('%(message)s'))
        _root_logger.addHandler(_engine_handler)


def log_deinit():
    """Deinitialize the logging system and remove Python engine handler."""
    global _engine_handler

    # Remove engine handler if it exists
    if _engine_handler is not None:
        _root_logger.removeHandler(_engine_handler)
        _engine_handler = None

    _root_logger.setLevel(logging.NOTSET)
    _if.log.log_deinit()


def enable_console():
    _if.log.enable_console()


def disable_console():
    _if.log.disable_console()


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
