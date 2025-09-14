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


def log_init():
    _if.log.log_init()
    _root_logger.setLevel(logging.DEBUG)


def log_deinit():
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
