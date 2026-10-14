#!/usr/bin/env python3

"""Tests for the refcounted holder in ``sim_estab.upkeep.log`` (``design_logging.md``
§2): the first holder initializes, the last tears down, the first holder's config wins,
and the count is one step from any thread.
"""

import threading

import pytest

import sim_estab._if as if_module
from sim_estab.config import LogConfig
from sim_estab.upkeep import log as sim_log


@pytest.fixture(autouse=True)
def released():
    """Every test starts and ends with nothing held."""
    assert sim_log.holder_count() == 0
    yield
    while sim_log.holder_count() > 0:
        sim_log.release()


def test_the_first_holder_initializes_and_the_last_tears_down():
    assert not if_module.log.log_is_init()
    sim_log.acquire(LogConfig())
    assert if_module.log.log_is_init()
    assert sim_log.holder_count() == 1
    sim_log.acquire(LogConfig())
    assert sim_log.holder_count() == 2
    sim_log.release()
    assert if_module.log.log_is_init()  # one holder left
    sim_log.release()
    assert not if_module.log.log_is_init()
    assert sim_log.holder_count() == 0


def test_release_without_acquire_raises():
    with pytest.raises(RuntimeError, match="without a matching acquire"):
        sim_log.release()


def test_the_first_holders_config_wins():
    sim_log.acquire(LogConfig(console=False))
    with pytest.warns(RuntimeWarning, match="first holder's configuration wins"):
        sim_log.acquire(LogConfig(console=True))
    assert sim_log.holder_count() == 2


def test_a_same_config_does_not_warn(recwarn):
    sim_log.acquire(LogConfig())
    sim_log.acquire(LogConfig())
    assert not [w for w in recwarn if issubclass(w.category, RuntimeWarning)]


def test_unapplied_fields_warn(tmp_path):
    """``level`` and ``session_dir`` are accepted ahead of the severity API and the
    per-session file sink (``design_logging.md`` §3), and say so."""
    with pytest.warns(RuntimeWarning, match="level='debug' is not applied yet"):
        sim_log.acquire(LogConfig(level="debug"))
    sim_log.release()
    with pytest.warns(RuntimeWarning, match="session_dir=.* is not applied yet"):
        sim_log.acquire(LogConfig(session_dir=tmp_path))


def test_acquire_and_release_are_safe_from_any_thread():
    """Many threads acquire and release at once; the count and the triad agree at the
    end."""
    rounds = 200

    def churn() -> None:
        for _ in range(rounds):
            sim_log.acquire(LogConfig())
            assert if_module.log.log_is_init()
            sim_log.release()

    threads = [threading.Thread(target=churn) for _ in range(8)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert sim_log.holder_count() == 0
    assert not if_module.log.log_is_init()
