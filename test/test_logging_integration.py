#!/usr/bin/env python3
"""Test Python logging integration with SimEstab C++ engine."""

import logging
from sim_estab.upkeep import log as sim_log

def test_python_handler_integration():
    """Test that Python logging forwards to C++ engine."""
    print("Testing Python logging handler integration...")

    # Initialize engine logging
    sim_log.log_init()
    sim_log.enable_console()

    # Test different logger names
    test_loggers = [
        logging.getLogger('sim_estab.network.tcp'),
        logging.getLogger('sim_estab.render.opengl'),
        logging.getLogger('sim_estab.physics.bullet'),
        logging.getLogger('myapp.module'),
        logging.getLogger(),  # root logger
    ]

    print("\n--- Testing different log levels ---")
    for logger in test_loggers:
        logger.info(f"Info message from {logger.name}")
        logger.warning(f"Warning message from {logger.name}")
        logger.error(f"Error message from {logger.name}")
        logger.debug(f"Debug message from {logger.name}")

    # Test hierarchical channel naming
    print("\n--- Testing hierarchical logging ---")
    logger = logging.getLogger('sim_estab.network.tcp.connection')
    logger.info("Connection established to server")
    logger.error("Connection timeout occurred")
    logger.critical("Network failure detected")

    # Test different formatting
    print("\n--- Testing different log formats ---")
    formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')
    sim_log._root_logger.handlers[0].setFormatter(formatter)
    logger.info("Test with custom formatter")

    # Reset to simple format
    sim_log._root_logger.handlers[0].setFormatter(logging.Formatter('%(message)s'))

    # Test severity level mapping
    print("\n--- Testing severity level mapping ---")
    logger.log(5, "Trace level message")  # Below DEBUG
    logger.log(15, "Between INFO and DEBUG")  # Between INFO and DEBUG
    logger.log(25, "Between INFO and WARNING")  # Between INFO and WARNING
    logger.log(35, "Between WARNING and ERROR")  # Between WARNING and ERROR
    logger.log(45, "Between ERROR and CRITICAL")  # Between ERROR and CRITICAL

    # Verify C++ handler is working
    print(f"\n--- Handler verification ---")
    print(f"Engine handler initialized: {sim_log._engine_handler is not None}")
    print(f"Root logger handlers: {len(sim_log._root_logger.handlers)}")
    print(f"Root logger level: {sim_log._root_logger.level} ({logging.getLevelName(sim_log._root_logger.level)})")

    print("\n--- Cleaning up ---")
    sim_log.log_deinit()
    print(f"Engine handler after deinit: {sim_log._engine_handler is not None}")
    print(f"Root logger handlers after deinit: {len(sim_log._root_logger.handlers)}")
    print(f"Root logger level after deinit: {sim_log._root_logger.level} ({logging.getLevelName(sim_log._root_logger.level)})")

    print("\nTest completed!")

if __name__ == "__main__":
    test_python_handler_integration()