#!/usr/bin/env python3

"""The free-threading guard of ``design_python_api.md`` §6.

``nanobind_add_module(... FREE_THREADED ...)`` declares the extension safe to run
without the GIL. Without it, a free-threaded interpreter silently re-enables the GIL
when the extension is imported, and every multi-threaded measurement after that is
wrong.
"""

import sys
import sysconfig

import sim_estab  # noqa: F401  (the import is the point: it loads the extension)


def test_the_extension_keeps_the_gil_disabled():
    if not sysconfig.get_config_var("Py_GIL_DISABLED"):
        assert sys._is_gil_enabled()  # a GIL build; nothing to guard
        return
    assert not sys._is_gil_enabled(), "importing sim_estab._if re-enabled the GIL"
