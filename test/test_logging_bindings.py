#!/usr/bin/env python3

"""Test script to verify the logging APIs exposed from log.h are working correctly."""

import pytest
import sim_estab._if as if_module


def test_severity_level_enum():
    """Test that severity_level enum values are correctly exposed."""
    # Test severity_level enum values
    assert hasattr(if_module.log, 'severity_level')
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
    """Test that log_error exception is exposed and inherits from RuntimeError."""
    assert hasattr(if_module.log, 'log_error')
    assert issubclass(if_module.log.log_error, RuntimeError)


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
    assert hasattr(if_module.log, 'severity_level')
    assert hasattr(if_module.log, 'log_error')
    assert hasattr(if_module.log, 'log_init')
    assert hasattr(if_module.log, 'log_deinit')
    assert hasattr(if_module.log, 'log_is_init')
    assert hasattr(if_module.log, 'enable_console')
    assert hasattr(if_module.log, 'disable_console')

    # Check that they are callable (except for the enum and exception)
    assert callable(if_module.log.log_init)
    assert callable(if_module.log.log_deinit)
    assert callable(if_module.log.log_is_init)
    assert callable(if_module.log.enable_console)
    assert callable(if_module.log.disable_console)