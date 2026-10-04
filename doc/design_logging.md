# Logging Design

This document describes the `sim_estab:log` subsystem: how it is built, and the rules for
starting, stopping and controlling it. Unlike the other design docs, most of it is
**implemented**. Parts marked *planned* are not.

Status: the C++ subsystem (§1) is built. The Python refcounted wrapper, file sinks, a
runtime severity API and the per-session file sink (§2, §3) are planned.

Terms are defined in [glossary.md](glossary.md).

Where the code lives:

- Interface: `src/libsim_estab/module/sim_estab--log.cppm` (partition `sim_estab:log`,
  re-exported by `sim_estab.cppm`)
- Implementation: `src/libsim_estab/src/log.cpp`
- Consumer macros: `src/libsim_estab/include/macro.h` (modules cannot export macros)
- Bindings: `src/sim_estab_ext/bind.cpp` → `sim_estab._if.log`

Conventions for channels, severity use and logging before a throw are in
`design_patterns.md` §7, not here. The subsystem is the reference example of the init/deinit
triad (`design_patterns.md` §2) and of the opaque wrapper with small-buffer storage
(`design_patterns.md` §5b).

---

## 1. Architecture (as built)

The subsystem wraps a Boost.Log core, hidden completely behind the module interface.

- **Lifecycle triad**: `log_init()` (may throw, idempotent), `log_deinit()` (`noexcept`,
  idempotent, allows a later `log_init`), and `log_is_init()`. One `detail::` mutex guards
  the mutable state. **`log_is_init()` takes no lock.** It is an acquire load of an atomic
  flag, because it runs on every emission in the process. A lock there would deadlock
  `log_deinit`, which emits its last record while holding the mutex, and would serialize
  every emission in the process (`design_patterns.md` §2).
  - `log_init` builds its state, sets the flag, releases the lock, and then emits "Logging
    subsystem initialized".
  - `log_deinit` emits "Logging subsystem shutting down", clears the flag, flushes, then
    tears down.
- **Sinks** are tracked in a `flat_map<sink_type, sink_ptr>`. Before building fresh state,
  `log_init` cleans up defensively: it flushes, clears the map, and removes stale sinks. The
  sink slots are `console`, `file`, `file_error` and `file_debug`. Only **console** is
  implemented; the file slots are reserved (*planned*, §3).
- **Console sink**: colored output (by severity) to `std::clog`, with auto-flush,
  microsecond timestamps, and the severity and hierarchical channel in the format. It is
  toggled with `enable_console()` / `disable_console()`. Both are idempotent and do nothing
  when the subsystem is not initialized.
- **Severity**: `severity_level { trace, debug, info, warning, error, critical }`, with
  values spaced 0, 10, 20, 30, 40, 50 so levels can be added in between without renumbering.
  The default core filter is `info` and above.
- **Emission**: library code calls the variadic `sim_estab_log(channel, severity,
  parts...)`. Each part must satisfy `LogStreamable`; a foreign type opts in with
  `SIM_ESTAB_LOG_ENABLE_OSTREAM(Type)` or `stream_via_ostream`. Consumers use the
  `SIM_ESTAB_LOG_*` macros from `macro.h`.
  - **The hot-path guard**: emission does nothing when `!log_is_init()`. That costs one
    atomic load and no lock.
  - **The guard is advisory.** A record that passes it while `log_deinit` runs may be
    dropped. The caller must stop emission before calling `log_deinit`, and `log_deinit`
    never waits for records in flight (`design_patterns.md` §2, §7).
  - **Dropping is safe**, because of Boost.Log rather than our lock. `push_record` locks
    weak pointers to the accepting sinks, so a sink removed during a push is skipped, and
    one still alive is kept alive through `consume()`. Clearing the flag first narrows the
    window: an emitter arriving during `log_deinit` skips at once instead of blocking
    through the whole teardown.
  - Emitting while holding another subsystem's lock is allowed: logging is the bottom of the
    lock hierarchy. Emitting from **inside a parallel phase** must use the buffered
    per-worker path instead (`design_modding.md` §5.2), which is not built yet.
- **Opaque wrappers**: `record`, `record_ostream`, `logger` and `logger_mt` wrap the
  Boost.Log types one to one, with small-buffer storage. Copy and move behave exactly like
  the wrapped types (`design_patterns.md` §5b).
- **Errors**: `log_error`, derived from `sim_estab_error`, exists and is bound to Python as
  `_if.log.log_error`, but no code throws it yet. Boost exceptions can escape `log_init` and
  `enable_console` directly.
- **Python surface**: mirrors the triad and the console toggles one to one, plus a concrete
  `log(channel, level, message)` wrapper.

## 2. Lifetime and runtime control (rules)

This section sets who starts and stops logging, and how it is controlled at run time. The
C++ triad does not change; the ordering lives above it. The Python parts are *planned*.
Today `sim_estab/upkeep/log.py` calls `log_init` and `log_deinit` directly, with no
refcount.

**Who starts logging, and when.**

- **Logging starts before every other subsystem and stops after the last**, outside the
  acquisition chain. Emission does nothing while the subsystem is not initialized (§1), so
  any other order discards the most important diagnostics: those on a construction-failure
  path, which `design_patterns.md` §4 and §6 rely on.
- **The Python layer makes the calls**, not the C++ `Engine` constructor:
  `sim_estab.upkeep.log.acquire(cfg)` and `release()` bracket native construction and
  release (`design_python_api.md` §2).
- **The Python wrapper is refcounted; the C++ triad stays idempotent.** `acquire` calls
  `log_init()` for the first holder, `release` calls `log_deinit()` for the last, and both
  do nothing in between. The count exists for `log_deinit`, not `log_init`. `log_init`
  already returns early when initialized, but `log_deinit` tears down unconditionally.
  Without a count, the first `close()` would tear down logging under an application that set
  it up for its own use. This is the `acquire` / `release` shape of `design_patterns.md` §3,
  applied in Python over the unchanged triad.
- **A module-level `threading.Lock` guards the count.** `acquire` and `release` take it
  around the count and the triad call it decides on. So "the first holder initializes" and
  "the last holder tears down" are each one step, from any thread, on both the GIL and the
  free-threaded build. An atomic count alone would not do it: a second `acquire` could see
  the count already raised and log before the first holder's `log_init` had finished. The
  lock is taken twice per `Engine` lifetime, so its cost does not matter.
- **The first caller's configuration wins**, with a warning. A later `acquire` with a
  different `LogConfig` does not reconfigure the sinks, following "immutable creation
  options, first caller wins" (`design_patterns.md` §3). `LogConfig` has three fields
  (`design_python_api.md` §3):

  | Field | Default | Meaning |
  |---|---|---|
  | `console` | `True` | the console sink of §1 |
  | `level` | `"info"` | the starting severity filter |
  | `session_dir` | `None` | the directory for the per-session file sink (below). `None` opens no per-session file |

**Per-session file sink.** It opens at the **freeze**, in `LogConfig.session_dir`, not when
the engine is constructed, because the freeze is the first moment a session identity exists
to name it after (`design_engine_core.md` §2.4 step 8). With no `session_dir`, there is no
per-session file. It is a sink toggle (§3), owned by the engine and
removed at `close()`, so it is separate from the refcount above. Its name adds a
**run-unique part** to the session identity. Every run of the same replay has the same
identity, so without that part repeated replay runs would overwrite or interleave one file.
This is safe because log naming is not part of the replay header (`design_engine_core.md`
§2.3).

**Runtime control.** Log level and sink toggles are **control calls**, never simulation
commands. They change no engine core state and are never recorded in the replay's input stream
(`design_engine_core.md` §1, `design_python_api.md` §3). A future
`Engine.set_log_level(...)` will forward to this subsystem, and a replay may run at any
verbosity.

**Mod channels.** Each mod logs on its own channel, `sim_estab.mod.<id>`
(`design_modding.md` §4.1). This keeps the hierarchical channel convention, so filtering by
mod uses the existing channel filter. The prefix is also the name of the `sim_estab.mod`
Python package (`design_python_api.md` §1). That is intended: a channel is a filtering
hierarchy, not an import path, and the two never resolve against each other.

## 3. Planned

- **File sinks**: fill the reserved `file`, `file_error` and `file_debug` slots, with
  `enable_file(...)` / `disable_file()` following the console toggle's shape. They are a
  general rotating file sink, an error-and-above sink, and an opt-in debug sink.
- **Runtime severity filter API**: `set_severity(level)`, global at first and later per
  channel prefix. The filter is currently fixed at `info` inside `log_init`; `LogConfig.level`
  sets the starting value once the wrapper exists.
- **Session integration**: the per-session file sink of §2, named from the session identity
  plus a run-unique part, opened at the freeze in `LogConfig.session_dir`
  (`design_engine_core.md` §2.4) and closed by `Engine.close()`.
- **Refcounted Python wrapper**: `acquire()` / `release()` in `sim_estab.upkeep.log`, with
  the count under a module-level `threading.Lock` (§2). The C++ triad needs no change.
