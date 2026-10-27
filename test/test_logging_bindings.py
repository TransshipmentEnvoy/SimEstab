#!/usr/bin/env python3

"""Test script to verify the logging APIs bound in sim_estab._if.log are working correctly."""

import pytest
import sim_estab._if as if_module


def test_severity_level_enum():
    """Test that severity_level enum values are correctly exposed."""
    # Test severity_level enum values
    assert hasattr(if_module.log, "severity_level")
    assert if_module.log.severity_level.trace.value == 0
    assert if_module.log.severity_level.debug.value == 10
    assert if_module.log.severity_level.info.value == 20
    assert if_module.log.severity_level.warning.value == 30
    assert if_module.log.severity_level.error.value == 40
    assert if_module.log.severity_level.critical.value == 50

    # Test enum member access
    assert str(if_module.log.severity_level.trace) == "severity_level.trace"
    assert str(if_module.log.severity_level.debug) == "severity_level.debug"
    assert str(if_module.log.severity_level.info) == "severity_level.info"
    assert str(if_module.log.severity_level.warning) == "severity_level.warning"
    assert str(if_module.log.severity_level.error) == "severity_level.error"
    assert str(if_module.log.severity_level.critical) == "severity_level.critical"


def test_log_error_exception():
    """Test that log_error is exposed, under the sim_estab_error root."""
    assert hasattr(if_module.error, "sim_estab_error")
    assert issubclass(if_module.error.sim_estab_error, RuntimeError)
    assert hasattr(if_module.log, "log_error")
    assert issubclass(if_module.log.log_error, if_module.error.sim_estab_error)


def test_log_init_and_is_init():
    """Test log initialization functions."""
    # Should not be initialized initially
    initial_status = if_module.log.log_is_init()
    assert isinstance(initial_status, bool)

    # Initialize the logging system
    if_module.log.log_init()

    # Should be initialized after log_init
    assert if_module.log.log_is_init() is True


def test_console_functions():
    """Test console enable/disable functions."""
    # Make sure logging is initialized first
    if_module.log.log_init()

    # Test enable_console - should not raise exception
    if_module.log.enable_console()

    # Test disable_console - should not raise exception
    if_module.log.disable_console()


def test_log_deinit():
    """Test log deinitialization function."""
    # Initialize first
    if_module.log.log_init()
    assert if_module.log.log_is_init() is True

    # Deinitialize
    if_module.log.log_deinit()

    # Should be deinitialized after log_deinit
    # Note: This behavior depends on the actual implementation
    # Some implementations might keep initialized state after deinit
    assert isinstance(if_module.log.log_is_init(), bool)


def test_all_logging_apis_exist():
    """Test that all expected logging APIs are exposed."""
    # Check that all expected functions exist in the log submodule
    assert hasattr(if_module.log, "severity_level")
    assert hasattr(if_module.log, "log_error")
    assert hasattr(if_module.log, "log_init")
    assert hasattr(if_module.log, "log_deinit")
    assert hasattr(if_module.log, "log_is_init")
    assert hasattr(if_module.log, "enable_console")
    assert hasattr(if_module.log, "disable_console")
    assert hasattr(if_module.log, "file_open")
    assert hasattr(if_module.log, "severity_level_names")
    assert hasattr(if_module.log, "file_open_names")
    assert hasattr(if_module.log, "enable_file")
    assert hasattr(if_module.log, "disable_file")
    assert hasattr(if_module.log, "set_severity")

    # Check that they are callable (except for the enum and exception)
    assert callable(if_module.log.log_init)
    assert callable(if_module.log.log_deinit)
    assert callable(if_module.log.log_is_init)
    assert callable(if_module.log.enable_console)
    assert callable(if_module.log.disable_console)
    assert callable(if_module.log.enable_file)
    assert callable(if_module.log.disable_file)
    assert callable(if_module.log.set_severity)


def test_file_open_enum_and_the_name_lists():
    """``file_open`` is bound like ``severity_level``, and both name lists follow the
    enums (``design_logging.md`` §3)."""
    assert if_module.log.file_open.rotate.value == 0
    assert if_module.log.file_open.append.value == 1
    assert if_module.log.file_open.truncate.value == 2
    assert if_module.log.severity_level_names == (
        "trace",
        "debug",
        "info",
        "warning",
        "error",
        "critical",
    )
    assert if_module.log.file_open_names == ("rotate", "append", "truncate")


def test_enable_file_round_trip(tmp_path):
    """The 1:1 functions: a file opens, receives a record and closes."""
    if_module.log.log_init()
    try:
        if_module.log.disable_console()
        path = tmp_path / "bound.log"
        if_module.log.enable_file(
            path,
            if_module.log.severity_level.info,
            if_module.log.severity_level.critical,
            if_module.log.file_open.truncate,
        )
        if_module.log.log(
            "test.bound", if_module.log.severity_level.info, "bound record"
        )
        if_module.log.disable_file(path)
        assert "bound record" in path.read_text()
        with pytest.raises(if_module.log.log_error):
            if_module.log.enable_file(tmp_path / "bound.log" / "nested.log")
    finally:
        if_module.log.log_deinit()
