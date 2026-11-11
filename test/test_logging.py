#!/usr/bin/env python3

"""Tests for the refcounted holder in ``sim_estab.upkeep.log`` (``design_logging.md``
§2, §3): the first holder initializes, the last tears down, the first holder's config
wins, the count is one step from any thread, and the first holder's level, session file
and ``file_list`` entries are applied. The bridge forwards each Python record at its
severity.
"""

import logging
import sys
import threading

import pytest

import sim_estab._if as if_module
from sim_estab.config import LogConfig, LogFile
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


def test_release_without_acquire_does_nothing():
    """``design_patterns.md`` §3: ``release`` at count 0 is a safe no-op."""
    sim_log.release()
    assert sim_log.holder_count() == 0
    assert not if_module.log.log_is_init()
    # the count did not go below 0: the next acquire is the first holder
    sim_log.acquire(LogConfig())
    assert sim_log.holder_count() == 1
    assert if_module.log.log_is_init()


def test_the_first_holders_config_wins():
    sim_log.acquire(LogConfig(console=False))
    with pytest.warns(RuntimeWarning, match="first holder's configuration wins"):
        sim_log.acquire(LogConfig(console=True))
    assert sim_log.holder_count() == 2


def test_a_same_config_does_not_warn(recwarn):
    sim_log.acquire(LogConfig())
    sim_log.acquire(LogConfig())
    assert not [w for w in recwarn if issubclass(w.category, RuntimeWarning)]


def test_the_session_file_opens_for_the_first_holder(tmp_path):
    """``session_dir`` opens ``session.log`` with the user's level as its floor, and the
    file starts with the header (``design_logging.md`` §3 "The session file")."""
    sim_log.acquire(LogConfig(console=False, session_dir=tmp_path))
    session = tmp_path / "session.log"
    assert session.exists()
    logging.getLogger("test.file").info("an info record")
    logging.getLogger("test.file").debug("a debug record")
    sim_log.release()

    text = session.read_text()
    assert "file sink opened: " in text
    assert "band info..critical, open mode rotate" in text
    assert "[INFO][test.file] an info record" in text
    assert "a debug record" not in text


def test_the_level_applies(tmp_path):
    """``level`` is the user's level: the session file's floor follows it."""
    sim_log.acquire(LogConfig(console=False, level="warning", session_dir=tmp_path))
    logging.getLogger("test.file").info("an info record")
    logging.getLogger("test.file").warning("a warning record")
    sim_log.release()

    text = (tmp_path / "session.log").read_text()
    assert "a warning record" in text
    assert "an info record" not in text


def test_file_list_entries_open_with_their_bands(tmp_path):
    """Each ``LogFile`` carries its own band, and the core filter follows the lowest
    floor, so a file below the user's level receives those records."""
    errors = tmp_path / "errors.log"
    everything = tmp_path / "all.log"
    sim_log.acquire(
        LogConfig(
            console=False,
            file_list=[LogFile(errors, floor="error"), LogFile(everything)],
        )
    )
    logging.getLogger("test.file").debug("a debug record")
    logging.getLogger("test.file").info("an info record")
    logging.getLogger("test.file").error("an error record")
    sim_log.release()

    error_text = errors.read_text()
    assert "an error record" in error_text
    assert "an info record" not in error_text
    assert "file sink opened" not in error_text  # the header is info
    all_text = everything.read_text()
    assert "a debug record" in all_text
    assert "an info record" in all_text
    assert "an error record" in all_text


@pytest.mark.parametrize(
    "levelno, severity",
    [
        (logging.DEBUG, "DEBUG"),
        (logging.INFO, "INFO"),
        (logging.WARNING, "WARNING"),
        (logging.ERROR, "ERROR"),
        (logging.CRITICAL, "CRITICAL"),
        (logging.CRITICAL + 10, "CRITICAL"),
        (logging.INFO + 5, "INFO"),
        (logging.DEBUG - 5, "DEBUG"),
    ],
)
def test_the_bridge_keeps_the_severity(tmp_path, levelno, severity):
    """The bridge forwards every Python record at the severity of its level
    (``design_logging.md`` §2): a standard level at the severity of the same name, a
    level between two standard levels at the lower one, a level below ``DEBUG`` as
    ``debug``, and a level above ``CRITICAL`` as ``critical``."""
    everything = tmp_path / "all.log"
    logger = logging.getLogger("test.bridge")
    logger.setLevel(1)  # the logger itself drops nothing, not even a level below DEBUG
    sim_log.acquire(LogConfig(console=False, file_list=[LogFile(everything)]))
    try:
        logger.log(levelno, "a bridged record")
    finally:
        sim_log.release()
        logger.setLevel(logging.NOTSET)

    assert f"[{severity}][test.bridge] a bridged record" in everything.read_text()


def test_rotation_at_acquire(tmp_path):
    """A re-used session path: the previous run becomes ``.01``."""
    for run in ("first run", "second run"):
        sim_log.acquire(LogConfig(console=False, session_dir=tmp_path))
        logging.getLogger("test.file").info(run)
        sim_log.release()
    assert "second run" in (tmp_path / "session.log").read_text()
    assert "first run" in (tmp_path / "session.01.log").read_text()


def test_a_failed_open_shuts_logging_down_and_raises(tmp_path):
    """A file that does not open: ``acquire`` shuts logging down again and raises
    ``log_error`` (``design_logging.md`` §3 "Python surface")."""
    blocker = tmp_path / "blocker"
    blocker.write_text("not a directory")
    with pytest.raises(if_module.log.log_error, match="cannot open"):
        sim_log.acquire(LogConfig(console=False, session_dir=blocker))
    assert sim_log.holder_count() == 0
    assert not if_module.log.log_is_init()


def test_set_severity_never_moves_a_band(tmp_path):
    """A file's band is fixed at its open; ``set_severity`` changes the console and the
    core filter only."""
    sim_log.acquire(LogConfig(console=False, session_dir=tmp_path))
    sim_log.set_severity("trace")
    logging.getLogger("test.file").debug("a debug record")
    logging.getLogger("test.file").info("an info record")
    sim_log.release()

    text = (tmp_path / "session.log").read_text()
    assert "a debug record" not in text
    assert "an info record" in text


def test_the_file_functions_validate_their_strings(tmp_path):
    sim_log.acquire(LogConfig(console=False))
    with pytest.raises(ValueError, match="floor must be one of"):
        sim_log.enable_file(tmp_path / "x.log", floor="bogus")
    with pytest.raises(ValueError, match="on_open must be one of"):
        sim_log.enable_file(tmp_path / "x.log", on_open="bogus")
    with pytest.raises(ValueError, match="level must be one of"):
        sim_log.set_severity("bogus")
    assert not (tmp_path / "x.log").exists()

    sim_log.enable_file(tmp_path / "x.log", floor="info", on_open="truncate")
    logging.getLogger("test.file").info("through the toggle")
    sim_log.disable_file(tmp_path / "x.log")
    assert "through the toggle" in (tmp_path / "x.log").read_text()


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


class _Terminal:
    """A stdin that is a terminal; ``input`` is replaced, so it is never read."""

    def isatty(self) -> bool:
        return True


def test_ask_for_confirm_confirms_when_nobody_can_answer(monkeypatch):
    """No console held, or stdin not a terminal: it confirms without asking."""
    monkeypatch.setattr("builtins.input", lambda prompt: pytest.fail("asked"))
    assert sim_log.ask_for_confirm() is True  # nothing held
    sim_log.acquire(LogConfig(console=False))
    monkeypatch.setattr(sys, "stdin", _Terminal())
    assert sim_log.ask_for_confirm() is True  # the console is off
    sim_log.release()
    sim_log.acquire(LogConfig())
    monkeypatch.setattr(sys, "stdin", None)
    assert sim_log.ask_for_confirm() is True  # no stdin


def test_ask_for_confirm_asks_on_a_terminal(monkeypatch):
    answers = iter(["maybe", "n", "y"])
    monkeypatch.setattr("builtins.input", lambda prompt: next(answers))
    monkeypatch.setattr(sys, "stdin", _Terminal())
    sim_log.acquire(LogConfig())
    assert sim_log.ask_for_confirm() is False  # "maybe" asks again, then "n"
    assert sim_log.ask_for_confirm() is True
