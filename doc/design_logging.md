# Logging Design

This document describes the `sim_estab:log` subsystem: how it is built, and the rules for
starting, stopping and controlling it. Unlike the other design docs, most of it is
**implemented**. Parts marked *planned* are not.

Status: the C++ subsystem (§1), the Python refcounted holder (§2) and the severity filter
(§3) are built. File sinks, the per-session file sink, the per channel prefix filter and
the per-worker buffer are planned (§4).

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
- **Sinks** are tracked per sink slot: the console slot holds one sink (§3). A file slot is
  planned (§4). Before building fresh state, `log_init` cleans up defensively: it flushes,
  clears the slot states, and removes every sink from the core.
- **The Boost.Log core is the process's one core, and this subsystem owns it.** `log_init`
  removes every sink from it and sets its filter; `log_deinit` disables it and removes
  every sink. Another Boost.Log user in the same process loses its sinks at either call.
  The console sink writes to `std::clog` through a non-owning pointer and never redirects
  the stream.
- **The core is enabled only while a sink is registered.** With no sink, Boost writes
  every record to stdout through its default sink, in its own format. So
  `disable_console()` turns the core off, and `enable_console()` turns it on again. A
  record emitted while the core is off is dropped before it is built.
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
- **Python surface**: mirrors the triad, the console toggles and `set_severity` one to one,
  plus a concrete `log(channel, level, message)` wrapper and the name list of
  `severity_level` (§3).

## 2. Lifetime and runtime control (rules)

This section sets who starts and stops logging, and how it is controlled at run time. The
C++ triad does not change; the ordering lives above it, in `sim_estab/upkeep/log.py`:
`acquire(config)` and `release()`. Its 1:1 functions over the triad stay beside them.

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
- **The Python side adds one handler and touches no other.** `log_init` adds
  `EngineLogHandler` to the root logger, which forwards every Python record to the C++
  subsystem, and sets the root level to `DEBUG`, saving the level it found; `log_deinit`
  removes that handler and restores the saved level. A handler the application or a
  process-level mod added stays as it is. The root level is set automatically because the
  bridge needs every record, and the application may set it again after `acquire`. Then
  `log_deinit` keeps the application's level and warns with a `RuntimeWarning` that it
  did not restore the saved one. The bridge is one way: a C++ record never reaches a
  Python handler.
- **The first caller's configuration wins**, with a warning. A later `acquire` with a
  different `LogConfig` does not reconfigure the sinks, following "immutable creation
  options, first caller wins" (`design_patterns.md` §3). `LogConfig` has three fields
  (`design_python_api.md` §3):

  | Field | Default | Meaning |
  |---|---|---|
  | `console` | `True` | the console sink of §1 |
  | `level` | `"info"` | the starting severity filter (§3) |
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
(`design_engine_core.md` §1, `design_python_api.md` §3). `Engine.set_log_level(...)`
enters the engine's operation lease and forwards to `set_severity` (§3), so a closed
engine raises instead of changing the level (`design_python_api.md` §8). A replay may run
at any verbosity.

**Mod channels.** Each mod logs on its own channel, `sim_estab.mod.<id>`
(`design_modding.md` §4.1). This keeps the hierarchical channel convention, so filtering by
mod uses the existing channel filter. The prefix is also the name of the `sim_estab.mod`
Python package (`design_python_api.md` §1). That is intended: a channel is a filtering
hierarchy, not an import path, and the two never resolve against each other.

## 3. File sinks and the severity filter

This section sets the sink slots under the toggles, the severity filter and the names from
the enums. The file sinks are planned (§4).

**Sink slots underneath the toggles.** The Python surface is the triad and the named
pairs. Underneath, each sink kind is a **sink slot**: a type, one for the console now and
one for the file once the file sinks exist (§4). A slot carries its fixed policy as
`static constexpr` members: its name and its flush rule. `log.cpp` maps each slot to its
Boost formatter (the console's colours), so Boost stays out of the interface. One pair of
function templates, `enable<Slot>(...)` and `disable<Slot>(...)`, implements every named
pair. The console slot holds one sink. The tracking container follows the slot list (§1).
The slot types and the pair are part of the C++ interface of `sim_estab:log`, so engine
code may use them directly. They are not bound to Python.

**The core filter and `set_severity`.** `set_severity(level)` sets the user's level,
global at first and later per channel prefix. The console carries the user's level as its
own filter, and the core filter follows it. `LogConfig.level` is the starting value, and
`Engine.set_log_level` forwards here (§2).

**Names from the enums.** `sim_estab:log` lists the enumerators of `severity_level` with
C++26 reflection (`std::meta::enumerators_of`). The name of a value, the parse of a name and
the console colour table come from that one list. `_if.log` exports the name list, and
`config.py` reads `LOG_LEVELS` from it. So each name is written once, in the enum. The
other enums stay with [Q73](open_question.md#q73-magic_enum-or-reflection-for-enum-names).

## 4. Planned

- **File sinks**: a file slot beside the console slot (§3), with `enable_file(...)` /
  `disable_file()` over the same slot pair. They are a general rotating file sink, an
  error-and-above sink, and an opt-in debug sink.
- **Session integration**: the per-session file sink of §2, named from the session identity
  plus a run-unique part, opened at the freeze in `LogConfig.session_dir`
  (`design_engine_core.md` §2.4) and closed by `Engine.close()`. Until then `acquire`
  accepts a `session_dir`, warns and opens nothing.

Not built: the per channel prefix filter of `set_severity`, which is global today (§3);
and the per-worker buffer for emission inside a phase (§1, `design_modding.md` §5.2).
