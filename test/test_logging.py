#!/usr/bin/env python3

"""Tests for the refcounted holder in ``sim_estab.upkeep.log`` (``design_logging.md``
§2, §3): the first holder initializes, the last tears down, the first holder's config
wins, the count is one step from any thread, and the first holder's level is applied.
"""

import logging
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


def test_the_level_applies_to_the_console(capfd):
    """``level`` is the user's level, the console's filter (``design_logging.md`` §3)."""
    sim_log.acquire(LogConfig(level="warning"))
    logging.getLogger("test.console").info("an info record")
    logging.getLogger("test.console").warning("a warning record")
    sim_log.release()
    err = capfd.readouterr().err
    assert "a warning record" in err
    assert "an info record" not in err


def test_session_dir_warns_until_the_session_file_exists(tmp_path):
    """``session_dir`` is accepted ahead of the session file (``design_logging.md`` §4),
    and says so."""
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


def test_no_sink_prints_nothing(capfd):
    """With the console off and no file, no sink is registered, so the core is off and
    Boost's default sink never prints to stdout (``design_logging.md`` §1)."""
    sim_log.acquire(LogConfig(console=False))
    logging.getLogger("test.quiet").warning("nobody hears this")
    sim_log.release()
    out, err = capfd.readouterr()
    assert "nobody hears this" not in out
    assert "nobody hears this" not in err


@pytest.fixture
def root_level():
    """The root logger level, put back after the test."""
    root = logging.getLogger()
    before = root.level
    yield root
    root.setLevel(before)


def test_release_restores_the_root_level(root_level):
    """``acquire`` sets the root level to ``DEBUG`` for the bridge, and the last
    ``release`` restores the level it found (``design_logging.md`` §2)."""
    root_level.setLevel(logging.WARNING)
    sim_log.acquire(LogConfig(console=False))
    assert root_level.level == logging.DEBUG
    sim_log.release()
    assert root_level.level == logging.WARNING


def test_release_keeps_a_level_the_application_set(root_level, recwarn):
    root_level.setLevel(logging.WARNING)
    sim_log.acquire(LogConfig(console=False))
    root_level.setLevel(logging.INFO)
    with pytest.warns(RuntimeWarning, match="does not restore WARNING"):
        sim_log.release()
    assert root_level.level == logging.INFO

    # set back to the saved level: nothing to restore, so no warning
    sim_log.acquire(LogConfig(console=False))
    root_level.setLevel(logging.INFO)
    recwarn.clear()
    sim_log.release()
    assert not [w for w in recwarn if issubclass(w.category, RuntimeWarning)]
