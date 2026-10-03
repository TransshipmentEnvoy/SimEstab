# Python API Design: Engine Lifecycle, Options, Main Loop

This document designs the Python side of `sim_estab`. It covers how the engine starts and
stops, how options are set, and how the main loop runs.

Status: designed, not implemented, except for the option objects of §3.
`src/sim_estab/config.py` holds `EngineConfig`, `HostPolicy` and `CommandPolicy` with their
decided M0 values. `bind.cpp` still exposes the placeholder top-level `init`, `deinit` and
`run` bindings, which this design replaces. The built C++ modules are `log`, `gpu`, `viz`,
`util` and `limits`. This design feeds the TODO items "design the basic engine structure"
and "session management".

Scope of v1: every engine view is `PRIVATE`. The multi-reader `SHARED` mode is deferred to
`design_engine_core.md` Appendix A, together with process-host engine views. So `engine.snapshot()`
copies from the default engine view (§7.2), and a process host has no engine view.

Terms are defined in [glossary.md](glossary.md). Companion docs:

- `design_engine_core.md`: the deterministic core, commands and snapshots.
- `design_patterns.md`: C++ lifecycle idioms. Its §8 binding conventions apply to everything
  here.
- `design_modding.md`: the mod system built on this API.

How to read this doc:

- Long sections start with a **TL;DR**.
- **Pattern** is the generic contract. **Example** shows a concrete sketch.
- All Python code in this doc is **pseudo-code**. It fixes shapes and contracts, not final
  names or signatures.
- A deviation from these rules needs a documented reason.

---

## 0. Decision record: Python owns the main loop

**TL;DR**

- The main loop is written in Python. The engine is driven like a library.
- All pacing-critical and heavy work happens inside coarse native calls that release the
  GIL.
- In a windowed session the ticks run on a C++ sim thread, apart from the Python frame loop
  (§4.3). Headless runs step the engine on the calling thread. Render stays on the Python
  loop, and mods run on their own threads.

The alternatives, and why they lost:

| Pattern | Runs the loop | Verdict |
|---|---|---|
| Launcher: blocking C++ `run()` (openage) | C++ | Rejected. It locks Python out while running, which breaks REPL, pytest and replay-harness workflows |
| Stepping: Python pumps the engine | Python | Chosen. It matches the convention of simulation libraries (MuJoCo, pybullet, gym `env.step`, the Panda3D task pump). It is the best fit for the verification harness of `design_engine_core.md` §2.3 |
| Background engine thread plus a control plane | C++ | Chosen for the tick loop of a windowed session: the sim thread (§4.3). Python keeps the frame loop, and the sim thread runs only ticks |
| Callback framework (`app.run(on_tick=...)`) | C++ | Rejected as the primary API. It re-enters the GIL at tick rate, and exceptions cross the C++ loop. Mods consume events and snapshots at their own mod host's pace instead (`design_modding.md` §4). No per-frame or per-tick callbacks exist. Whether every mod must still be paced every tick is open ([Q2](open_question.md#q2-must-every-mod-be-paced-every-tick)) |

A survey of openage, Panda3D, MuJoCo and Godot found one deciding factor. Engines that ship
a game embed a script VM and keep the loop native. Projects that are a Python package put
the loop in Python. SimEstab is a Python package. Unlike openage, Python is both the host
and the mod VM, so no C++-to-Python callback machinery is needed at all.

One standing rule comes from openage's one-way boundary:

- **`libsim_estab` stays free of Python, permanently.** It never links Python, never
  includes Python headers and never calls back into the interpreter. All traffic between
  Python and C++ goes through the `_if` nanobind extension.

## 1. Package layout

The package is a thin native extension with a pure-Python layer above it. Each module owns
one part of this design.

```
src/sim_estab/
  __init__.py     # DLL dir setup (Windows), re-exports
  _if...so        # nanobind extension: 1:1 mirror of the C++ API (§8 of design_patterns.md)
  engine.py       # Engine: lifecycle + pump primitives (§2, §4)
  config.py       # EngineConfig frozen dataclass + validation (§3)
  command.py      # typed command builders, batch submit (§7.1)
  snapshot.py     # zero-copy snapshot views (§7.2)
  event.py        # event types, drain/dispatch bus (§7.3)
  loop.py         # reference loops: windowed (sim thread) / headless / replay (§4)
  mod/            # mod manager — see design_modding.md
  upkeep/         # process-wide subsystems that outlive any Engine:
                  #   log.py = refcounted acquire/release over the C++ triad (§2)
```

Rules:

- `_if` stays a thin 1:1 mirror of the C++ API, following the binding conventions of
  `design_patterns.md` §8. Anything Pythonic lives in the pure-Python layer above it, never
  in `bind.cpp`: context managers, dataclasses, defaults, keyword conveniences.
- The pure-Python layer never goes around `_if`: no ctypes, no direct `.so` loading.

## 2. Engine lifecycle

**TL;DR**

- There is one `Engine` object per process. Construction acquires resources and `close()`
  releases them. This is the Python face of the RAII context pattern (`design_patterns.md`
  §4).
- Options are frozen at construction (§3). The **session** is frozen later, by
  `start_session()` (`design_engine_core.md` §2.4). These are two different moments.
  Content, mods and dynamic columns arrive between them.
- Only the owner thread, the thread that constructed the engine, may make lifecycle calls.
  An always-on check enforces this.
- Every native call that may overlap `close()` first takes an **operation lease**. `close()`
  refuses new leases and waits, under the shutdown deadline, for running calls to finish.
  Only then does it release native memory.
- Everything that can outlive a tick and still touch engine memory is **registered with the
  engine**: mod hosts, and engine view blocks held by live array views. `close()` unwinds these
  registrations before it releases anything native. It alone decides the shutdown order.

### Pattern

```python
# pseudo-code
with sim_estab.Engine(config) as engine:             # ctor = acquire; state = configuring
    mods = sim_estab.mod.load_mods(engine, policy)   # discovery, verification, native load,
                                                     #   column + engine view registration. NO freeze
    engine.start_session()                           # the freeze — always explicit, always
                                                     #   the caller's, mods or no mods
    mods.start()                                     # spawn hosts; they may now read the
                                                     #   tick-0 snapshot in on_load
    engine.run_sim_async()                           # windowed: ticks run on the sim
                                                     #   thread from here on (§4.3)
    run_windowed(engine, mods)                       # §4
# __exit__: mods.stop() -> stop_sim_async() -> close()
#   mods.stop() does NOT end the session; close() does.

# with no mods, the two mod lines are simply absent:
with sim_estab.Engine(config) as engine:
    engine.start_session()
    engine.run_sim_async()
    run_windowed(engine, None)
```

**No loading function starts a session, and no unloading function ends one.** The extra
lines above are intended. `start_session()` is the moment that fixes session identity
(`design_engine_core.md` §2.4), and that moment must not depend on which loader happened to
run. The rule covers mod loading, replay loading, content loading and native-mod loading
alike. It is also symmetric. Stopping mod hosts, draining inboxes and running `on_unload`
leave the session running; only `close()` ends it.

Rules:

- The constructor may raise translated subsystem errors (§8). It runs the full acquisition
  chain through the C++ RAII contexts. A failure leaves nothing acquired, because the C++
  constructor-rollback rule undoes it. The constructor acquires native resources only. It
  never loads mods and never runs mod Python.
- **Logging is acquired around the chain, not inside it.** The Python constructor calls
  `sim_estab.upkeep.log.acquire(config.log)` before it enters the native constructor. It
  calls `release()` after the native release. So logging is live before the first
  acquisition and ends after the last release. Otherwise every diagnostic on the
  construction-failure path would go to an uninitialized subsystem and be lost, and §8
  builds its error story on that path.

  The wrapper is reference-counted in Python. `acquire` initializes logging for the first
  holder, and `release` shuts it down for the last. An application that configured logging
  before constructing the engine keeps its sinks. The engine releases exactly once, on the
  first `close()`. `close()` is idempotent (below), and a count decremented by a repeated
  close would tear down logging under a running application. The C++ triad is unchanged
  (`design_logging.md` §2). Whether the count is thread-safe on free-threaded builds is open
  ([Q19](open_question.md#q19-is-the-python-logging-refcount-thread-safe)).
- **Two engine states come before the loop** (`design_engine_core.md` §2.4). The constructor
  leaves the engine `configuring`: it holds resources but no world. `start_session()`
  freezes the session and moves the engine to `running`.

  | In `configuring` | Calls |
  |---|---|
  | legal | `load_content`, `load_native_mod`, `register_hosts`, `register_view`, `load_replay`, `poll_input`, control calls (§3), `state` |
  | raises `EngineStateError` | `step`, `render`, `run_sim_async`, `submit*`, `outcome*`, `snapshot()`, `view()`, `checksum()`, `drain_events`, `ModBus.start` |

  `start_session()` is owner-thread-only (§6). It runs the ordered freeze of
  `design_engine_core.md` §2.4. It raises `ModLoadError` or `ReplayIdentityError` (§8)
  rather than start a session it cannot describe. It is a transition, not an idempotent
  setter. Called in any state other than `configuring`, it raises `EngineStateError`, so a
  second call is reported, not silently ignored.

  **The freeze is always explicit, never implied by the first pump.** Identity errors belong
  at the freeze. Finding them on frame 1, after the window is up and mod hosts are live,
  gains nothing but one saved line. **Nothing else calls `start_session()`**, and this is a
  rule, not a convention. A loader that froze would make the freeze's place in the sequence
  depend on which loader ran. It would also have to spawn mod hosts, because spawning must
  follow the freeze (`design_modding.md` §6).
- **Registered dependants.** The engine holds the set of registered mod hosts
  (`design_modding.md` §6) and the set of engine views (§7.2). Registration gives a dependant the
  right to be stopped, drained or detached at shutdown, instead of being torn out from
  under. The sets fill at different times, in this order:
  - the mod bus is registered during `configuring`, before any mod host exists;
  - Engine views are registered before the freeze, which closes their set (`design_engine_core.md`
    §2.4 step 3a);
  - mod hosts spawn on `mods.start()`, after the freeze;
  - held blocks can build up only while `running`, because reading an engine view raises before
    that.
- **Shutdown is a chain of three steps**, in this order. The order is forced, not a matter
  of style:

  1. **Stop registered mod hosts** with `mods.stop()`. It revokes submission endpoints,
     stops feeding inboxes and snapshots, drains the inboxes, runs `on_unload`, and joins
     under a deadline (`design_modding.md` §6). This step runs with the GIL held, because
     `on_unload` is Python. It does not end the session: the session is still `running` when
     it returns, and steps 2 and 3 end it. A caller may call `mods.stop()` on its own
     without closing the engine. How the stopped mods then leave the gate is open
     ([Q9](open_question.md#q9-how-does-a-stopped-participant-leave-the-gate)).
  2. **`stop_sim_async()`**: the staged native join (§4.3) and the error rendezvous. It may
     raise. It runs with the GIL released. It ends ticking for the session: the engine is
     `stopped`, and no tick runs again. A session with no sim thread has nothing to join;
     the call only sets the stop.
  3. **`close()`**: close the operation lease and wait for running calls, detach retained
     `PRIVATE` engine view blocks, then release native resources. It never raises.

  Steps 1 and 2 cannot swap. The step that stops mod hosts needs the GIL to make progress,
  and the join step must release it. Joining first would starve every thread host for the
  whole join window. That would guarantee the deadline expiry the join tries to avoid
  (§4.3).
- **`close()` never raises and is idempotent**, mirroring "release and destroy are
  `noexcept`". An async error still pending at close is logged prominently, never raised
  (§8). The raising rendezvous points are `stop_sim_async()` and the pump calls. `close()`
  also never blocks indefinitely. Every wait inside it has a deadline, and an expiry leads
  to the `failed` path below, not to a hang. It performs steps 1 and 2 itself if the caller
  has not, so a bare `close()` is always a complete shutdown.
- **Step 3 closes the operation lease.** The Python `Engine` wrapper owns a small control
  block `{ atomic access_state, atomic active_operations, mutex, condition_variable }`. It
  outlives every native resource it guards. Every native call except `close()` takes the
  operation lease of §6. `close()` changes `OPEN -> CLOSING` with a compare-exchange. That
  transition is the linearization point: after it, no new call may enter. `close()` then
  waits, under the shutdown deadline, for `active_operations == 0`. Only a zero count allows
  detach and native release. A timeout enters the terminal `failed` state and disarms the
  native contexts. The resources are then kept, not freed under a running call. The wait
  releases the GIL; Python mod host teardown has already finished in step 1.

  The `Engine`, every engine view or snapshot handle that can re-enter it, and every `ModContext`
  share ownership of this control block. Native pointers live behind it and are cleared only
  after the wait. So a stale handle can still observe `CLOSING`, `CLOSED` or `FAILED`
  without dereferencing a destroyed wrapper. In the `failed` path, which disarms, the
  control block and the native graph are kept together.
- `__enter__` delegates to construction. `__exit__` runs the three steps above, and step 3
  is unconditional, in a `finally`. If the `with` body is already unwinding an exception, a
  stop-time error is logged instead of raised, so the body's exception is never masked. Code
  that does not use `with` calls the steps explicitly. Relying on GC finalization is
  unsupported; finalization warns if the engine was never closed.
- **One live `Engine` per process.** The wrapped resources (the SDL context, the GPU device)
  are per-process singletons. A second construction raises `EngineExistsError` (§8).
- `Engine` cannot be copied or pickled.
- **Lifecycle calls are owner-thread-only.** Construction, `close()` and every `run_*_async`
  or `stop_*_async` call (§4.3) run on the **owner thread**: the thread that constructed the
  `Engine`. That is the window-creating thread, which SDL requires for swapchain
  acquisition. For windowed configs SDL also requires it to be the main thread
  (`SDL_CreateWindow`). The check compares a stored thread id, never a platform heuristic
  for the main thread.

  **The check is always on and raises `EngineThreadError`.** It is not debug-only. Each
  checked call is O(1) per frame (§5 rule 1), and a debug-only check would leave an ordinary
  Python mistake as undefined behaviour in the core. Below the binding, the C++
  abort-on-invariant rule is unchanged. How this combines with `close()` never raising is
  open ([Q13](open_question.md#q13-can-close-be-called-from-another-thread)).
- **A call after closing raises.** A call that begins once closing has started, or after
  close, raises `sim_estab.EngineClosedError` instead of aborting. This is a *documented
  deviation* from the C++ rule "programming errors abort" (`design_patterns.md` §6). On the
  Python surface it is a common, recoverable user error, and an exception is the idiom. The
  abort rule still applies below the binding, for real invariant violations. This covers
  snapshot handles too. After `close()`, a handle raises `EngineClosedError` on anything
  that would re-enter the engine. The read-only array memory it already handed out stays
  valid (§7.2).

Engine states are explicit and can be queried. The diagram shows only three of the `failed`
entry paths; the list below it has all of them.

```
created ─(ctor ok)─► configuring ─(start_session ok)─► running ─(run_sim_async)─► running+sim-async
   ▲                  │                                 │                                  │
   │                  │                                 │                                  ├─(join timeout)─► failed
   │                  │                                 │                                  │ (stop_sim_async ok)
   │                  │                                 │                                  ▼
   │                  │                                 ├──(stop_sim_async ok)────────► stopped
   │                  │                                 ├──(host abandoned at stop)► failed
   │                  ├─(load/freeze fails)─► load_failed   (terminal)
   │                  │                                 │
   └── raise on reuse └──(close / __exit__)──────────┴──► closing ─(drain ok)─► closed
                                                                    └─(drain timeout)─► failed
```

- **`load_failed` is terminal but clean.** It is the only state that separates "this session
  could not be described" from "this engine could not be stopped". A load or freeze failure
  leaks nothing and poisons no process, and `close()` releases normally. To recover, fix the
  policy and construct a new `Engine`. That is legal, because `closed` is not `failed`. The
  state exists so that `closed` never stands in for a session that never started, and so
  that a partly loaded session has no state to live in (`design_modding.md` §6).

  It is entered only once the engine has actually been touched. A failure in the policy
  stage of `load_mods` (a bad path, a missing signature, a denied capability) leaves the
  engine `configuring`, and `load_mods` can be retried. That whole stage runs before
  `register_hosts` (`design_modding.md` §6).

- **`stopped` ends ticking, not the engine.** `stop_sim_async()` enters it, whether or not a
  sim thread ran. No tick runs again in the session, so `step()` and `run_sim_async()` raise
  `EngineStateError` (§8). Reads still work: `snapshot()`, `checksum()`, `drain_events()` and
  `outcome()`. `close()` releases as usual. To inspect a live session without ending it,
  pause and step instead (§4.3).

- **`failed` is a terminal state with seven entry paths, in two classes.**

  **Live execution that will not stop or finish safely.** Five paths:
  - the sim-thread join timed out (§4.3);
  - a participant's gate deadline expired a second time under `on_expiry = FAIL`
    (`design_engine_core.md` §3.3);
  - an endpoint revocation timed out while waiting for submits already admitted
    (`design_engine_core.md` §5.1, `design_modding.md` §6). What a process host does here is
    open ([Q46](open_question.md#q46-process-host-drain-timeout));
  - a thread host was abandoned at shutdown step 1. Thread hosts are cooperative only: a
    host stuck in native code cannot be killed in-process, so it is logged and abandoned
    (`design_modding.md` §6). An abandoned host may still hold an engine view block and still call
    `ctx.submit()`. So abandonment moves the engine to `failed`; it is not a warning that
    shutdown then ignores. Process hosts never take this path. They can be killed, so their
    deadline ends in a kill, not abandonment. The kill also unlinks the host's engine view
    shared-memory object once process hosts have engine views (`design_engine_core.md` Appendix A);
  - the operation-lease wait timed out in shutdown step 3. That shows a running call may
    still be touching native resources.

  **A resource the session cannot continue without.** Two paths:
  - GPU device loss (`design_engine_core.md` §5). The GPU is in the peripheral domain, so
    core state is untouched and the session can still be replayed;
  - commit failure in the terminal commit (`design_data_container.md` §2.2). Memory is
    reserved at each object type's cap, and the commit commits the pages it needs before it
    changes anything. So a failure leaves core state whole at tick N, and the tick is simply
    abandoned. It is never turned into a rejection. The freeze uses the same order:
    everything that can fail happens before anything is visible (`design_engine_core.md`
    §2.4).

  Device loss and commit failure are specified in other documents. They are listed here
  because this is where the `failed` paths are collected. The table below is written for the
  live-execution class: leak rather than free, because a live thread may still touch
  anything. For the resource class it is cautious but harmless, since nothing is running
  that could be surprised. Whether the resource class deserves its own terminal state is
  open ([Q17](open_question.md#q17-should-resource-failures-have-their-own-terminal-state)).

  In `failed`, nothing a surviving thread can reach is destroyed. This is the Python face of
  a disarmed RAII context (`design_patterns.md` §4): `close()` clears its witnesses without
  releasing anything.

  | What | Behaviour in `failed` |
  |---|---|
  | core state, command rings, engine view blocks | kept alive (leaked), not freed under a running thread |
  | logging | `critical`, naming exactly what was leaked |
  | `engine.state` | reports `failed` |
  | any further API call | raises `sim_estab.EngineFailedError` |
  | `close()` | a no-op that never raises |
  | a new `Engine` | still raises `EngineExistsError`: the process is poisoned |
  | shared SDL and GPU reference counts | not dropped. A later acquire would hand a live device to a process that lost track of one (`design_patterns.md` §3) |

  `on_failed_stop` (§3) chooses between raising and terminating the process. Which of the
  seven paths it covers is open
  ([Q65](open_question.md#q65-which-failed-paths-does-on_failed_stop-cover)). `closed` is
  reachable only through a clean shutdown. The API never reports `closed` to mean "gave up".

## 3. Options

**TL;DR**

- There is one frozen config object. It is validated in Python and converted once at the
  boundary.
- Anything that changes core state at runtime is a **command**. Runtime tuning that never
  touches core state is a **control call**, and it is never recorded.

### Pattern

```python
# pseudo-code; src/sim_estab/config.py is the implementation and holds these values
@dataclass(frozen=True, slots=True)
class HostPolicy:                        # engine-enforced, needed long after loading;
                                         #   values decided in doc/design_limits.md §1.1
    shutdown_deadline: float = 5.0       # seconds, one budget per blocking shutdown stage
                                         #   (five stages, listed below)
    ipc_deadline: float | None = None    # hot-path submit round trip -> host_error
                                         #   (design_modding.md §4.3). Undecided; None =
                                         #   unbounded; when set it must exceed one tick
                                         #   (Q43). It does NOT bound an outcome wait
    mod_retry_limit: int = 0             # quarantine: 0 = stay disabled, N = re-spawn up
                                         #   to N times (design_modding.md §4.2)
    max_inbox_size: int = 1024           # engine cap on the manifest's inbox_size

@dataclass(frozen=True, slots=True)
class CommandPolicy:                     # doc/design_limits.md §2
    drain_interval_ticks: int = 8        # D: ticks between drain_events() calls that the
                                         #   event ring is sized for
    source_capacity: int = 64            # commands ONE endpoint may hold for ONE tick,
                                         #   when none is declared
    host_source_capacity: int = 256      # source 0's own capacity
    peer_source_capacity: int = 256      # a peer endpoint's capacity
                                         # Not fields and not properties: ring depth
                                         #   ((margin + 1) x capacity), C (sum of
                                         #   capacities) and the event ring size (C x D).
                                         #   All three are computed at the freeze (§7.1).
                                         #   There is no drain quota and no
                                         #   ring_capacity_ticks

@dataclass(frozen=True, slots=True)
class EngineConfig:
    headless: bool = False
    tick_rate: int = 30                  # fixed sim Hz; the tick counter IS time
                                         #   (doc/design_limits.md §1)
    window: WindowConfig = ...           # not in config.py yet; fields open (Q18)
    log: LogConfig = ...                 # not in config.py yet; fields open (Q18)
    host_policy: HostPolicy = field(default_factory=HostPolicy)
                                         # what the ENGINE enforces on mod hosts (above);
                                         #   which mods load is ModPolicy, an argument to
                                         #   load_mods (design_modding.md §3.1)
    command_policy: CommandPolicy = field(default_factory=CommandPolicy)
                                         # endpoint capacity and D (§7.1)
    entity_capacity: Mapping[str, int] = field(default_factory=dict)
                                         # cap overrides, per object type, each 1..2**32-1.
                                         #   Empty = every type keeps its [[=cap(N)]], or
                                         #   the default 2**24. Every cap is closed at the
                                         #   freeze and joins session identity
                                         #   (design_data_container.md §2.1, §2.2)
    seed: int | None = None              # None -> generated, then recorded
    on_failed_stop: str = "raise"        # "raise" | "terminate" (§4.3)
    projection_warn_bytes_per_second: int = 4_000_000_000
                                         # publish bandwidth above which the engine warns,
                                         #   naming the rate AND THE ENGINE VIEW; checked per engine view,
                                         #   not against the session total, because
                                         #   every remedy belongs to one engine view. Uncapped
                                         #   by design and measured instead
                                         #   (design_engine_core.md §3.1,
                                         #   doc/design_limits.md §5)

engine = Engine(config)                  # the only place config crosses the boundary
```

The sketch matches `src/sim_estab/config.py`, which is the implementation. `window` and
`log` are the only fields not in `config.py` yet
([Q18](open_question.md#q18-what-goes-in-windowconfig-and-logconfig)).

Rules:

- Validate everything before touching native code. Raise `ValueError` with a specific
  message. This mirrors "validate arguments up front" (`design_patterns.md` §6).
- The config is immutable after construction. This is the Python face of "immutable creation
  options, first caller wins" (`design_patterns.md` §3). Only one `Engine` exists (§2), so
  no first-caller conflict can arise, and a second construction raises `EngineExistsError`
  anyway (§8).

  `log` is the one exception. Logging is reference-counted, and the application may already
  hold it. Then a different `LogConfig` is warned about and ignored, not applied. This is
  "first caller wins" in the literal sense of `design_patterns.md` §3 (`design_logging.md`
  §2).
- **There are two mod-related policies, and they are different objects.** `HostPolicy` is
  what the engine enforces: the shutdown budget, the IPC deadline, quarantine retries and
  the inbox cap. The loader knows none of these. `close()` needs all of them, whether or not
  a mod was ever loaded.
  - **One shutdown number, reused by every stage.** `shutdown_deadline` bounds each of five
    blocking shutdown stages on its own:
    1. the wait for running submits when an endpoint is revoked;
    2. a mod host's inbox drain and `on_unload`;
    3. the mod host or sim-thread join;
    4. confirming death after a process kill;
    5. the final operation-lease wait in `close()`.

    It is one budget per stage, not one total across the five. No use site needs the stages
    to differ, and together they form one escalation staircase. The sim thread's staged join
    works the same way: one timeout applied twice (§4.3). `ipc_deadline` stays separate
    because it is not part of that staircase. It bounds a per-call round trip on the hot
    path, and its expiry produces `host_error`, not abandonment.
  - **`ipc_deadline` does not bound an outcome wait, and could not.** A process mod reading
    what its command did is waiting for a tick to run. That wait lasts until the named tick
    has run (§7.1), which depends on the session, not on the transport. A transport deadline
    would expire it whenever the session was slow to reach that tick. So the outcome wait is
    its own operation. It can be cancelled, and its expiry means *still waiting*, never
    *kill the mod* (`design_modding.md` §4.3, `design_engine_core.md` §5.1).

    `ipc_deadline` defaults to `None`, which leaves the round trip unbounded. A value, when
    given, must exceed one tick. `EngineConfig` checks this, because the tick rate lives
    there. `design_limits.md` §1.1 gives the input delay as its ceiling. Its value and its
    constraints are open ([Q43](open_question.md#q43-ipc_deadline-value-and-constraints)).
  - **Quarantine is one number, not a mode plus a number.** `mod_retry_limit` alone decides
    it: 0 keeps a failed mod disabled, and N re-spawns it up to N times. A separate mode
    such as `on_mod_error="disable"` would say the same as `retry_limit=0`, and the pair
    would allow the contradiction `("disable", retry_limit=3)`.
  - There is no snapshot-refresh setting here. A mod's snapshot cadence is a property of its
    engine view, declared where the engine view is registered (§7.2).

  `ModPolicy` (`design_modding.md` §3.1) decides which mods exist and what they may do:
  paths, signatures, allowlist, denied capabilities, host override. It is an argument to
  `load_mods`, never a config field. It drives filesystem discovery and per-mod
  verification: Python policy code that fails one mod at a time. The constructor could not
  report that under the constructor-rollback rule (§2). Neither type is interactive. Nothing
  in the loading path asks the user a question.
  - The split follows the existing loading split: Python owns policy, and the engine owns
    mechanism (`design_modding.md` §5.1).
  - **Policy selects; the freeze records the selection.** No `ModPolicy` field appears in
    the session header. The *outcome* does: the manifest list and its hashes
    (`design_engine_core.md` §2.3, §2.4). So splitting the type cannot add a second identity
    input.
  - Both are frozen dataclasses. `HostPolicy` crosses the boundary once, with the rest of
    the config. `ModPolicy` never crosses it.
- Runtime changes split along a bright line (`design_engine_core.md` §1):
  - **Sim commands** (§7.1) are consumed by tick functions, change core state and are
    recorded in the input stream. They are the only way the world changes.
  - **Control calls** are engine API: `engine.set_log_level(...)`, `engine.pause()` and
    `resume()`, `engine.set_time_scale(...)`. They tune the loop and the peripherals and
    never touch core state. They are never recorded. A replay reproduces state bit-exactly,
    and the operator stays free to pause, fast-forward or change log levels during playback.
- **`entity_capacity` overrides caps; every object type has one.** A cap is the most
  entities of one type alive at one time. It comes from `[[=cap(N)]]` in the schema, or is
  the default 2²⁴ (`design_data_container.md` §2.1). An entry here sets or overrides it, from
  1 to 2³²−1.
  Lower a cap where a bound is wanted *as a rule*: a scenario with a fixed settlement limit,
  or a CI run that should not balloon.
  - **A cap changes what the simulation does.** So every cap is closed at the freeze and
    recorded in session identity. A create at the cap is rejected deterministically. A
    replay against a different cap is rejected by the header instead of diverging.
  - **Memory is reserved at the cap and committed as the pool fills.** A large cap costs
    address space, not memory (`design_data_container.md` §2.2). `command_policy` bounds
    *submitted commands*, and a core mod creates entities inside its tick function without
    submitting any, so the cap is what bounds world size. When a commit fails, the result
    is session-fatal, never a rejection. A rejection driven by machine memory would make
    two machines replaying one stream diverge by how much RAM they had
    (`design_data_container.md` §2.2).
- **`tick_rate` is not the speed setting, and must not become one.** It defines what one
  tick *means*. A unit moving one cell per tick moves at a different world speed if the rate
  changes. So changing the rate changes outcomes, not pacing. That is why it is frozen at
  construction and sits in the replay header (`design_engine_core.md` §2.3). **Game speed is
  the wall-clock rate at which ticks run**, and it sits outside the header. The whole speed
  range is control calls:

  | Want | Call | Effect on the sim |
  |---|---|---|
  | slower or faster | `set_time_scale(x)` | runs `x ×` real time's worth of ticks |
  | unbounded ("no waiting") | `set_time_scale(None)` | runs ticks continuously, with no wall-clock pacing and no catch-up clamp (§4.3) |
  | pause | `pause()` | runs zero ticks; the loop keeps running |
  | one tick | `step(1)` while paused (§4.3) | advances exactly one tick |

  All four produce bit-identical state for the same command stream. They change *when* you
  look, never *what happened*. A replay can be watched at any of them.
  - Escape hatch: game speed, or something like it, may one day be wanted *as game state*,
    for example a mod that slows time in the world. Model it then as core state changed by a
    real command. Never smuggle control calls into the input stream.
- `on_failed_stop` chooses what a `failed` entry path (§2) does next. It covers the
  sim-thread join timeout and an abandoned thread host. Which of the other paths it covers
  is open
  ([Q65](open_question.md#q65-which-failed-paths-does-on_failed_stop-cover)).
  - `"raise"` (the default) raises `EngineFailedError` and enters the terminal `failed`
    state. This suits interactive hosts that want to save unrelated work first.
  - `"terminate"` hard-aborts the process after logging. This suits CI and headless runs,
    where a hung run must die visibly instead of pretending to clean up.
- **`"terminate"` cleans up out-of-process state before aborting**, in this order:

  1. kill every process host;
  2. confirm each death through `waitpid` or `pidfd`;
  3. unlink each mod host's engine view shared-memory object (`design_modding.md` §4.3). This
     applies only once process hosts have engine views (`design_engine_core.md` Appendix A). In v1
     a process host has no engine view, so there is nothing to unlink;
  4. log `critical`;
  5. abort.

  Thread hosts and in-process memory need nothing, because they die with the process. Only
  child processes and named shared-memory objects outlive it. Orphaned mod hosts holding
  mapped engine view segments would pile up across runs, which is the failure CI notices last. The
  cleanup is safe under the very condition that triggers `terminate`. What would not stop is
  an in-process thread, and killing child processes touches nothing it owns.
- Conversion to the bound C++ options struct happens exactly once, inside the `Engine`
  constructor. No config state is duplicated on the Python side afterwards; queries go to
  the engine.

## 4. Main loop

**TL;DR**

- The sim uses a fixed tick ("Fix Your Timestep"). Render presents the latest published
  snapshot, with no interpolation (`design_engine_core.md` §3). The accumulator and the wall
  clock live in C++, not in Python.
- The loop crosses the boundary O(1) times per frame.
- A windowed session runs its ticks on the C++ sim thread. The Python frame loop submits,
  drains events, feeds the mod bus and renders, and never runs a tick (§4.1). A headless run
  advances with `step(n)` on the calling thread (§4.2). §4.3 gives the rules for both.
- **The frame loop never waits on the engine; it polls** (§4.3).

### 4.1 Pattern: the reference windowed loop

The windowed loop runs on the owner thread, while the sim thread runs ticks beside it. Once
per frame it pumps input, commands, events and rendering.

```python
# pseudo-code — loop.run_windowed(); the caller started the sim thread (§2)
def run_windowed(engine, mods):              # `mods`: the engine-registered bus (§2),
                                             #   or None when no mods are loaded
    open_handles = []                        # host-app commands whose tick has not run
    while not engine.stop_requested():
        inputs = engine.poll_input()         # SDL input/window events; owner thread
        open_handles += engine.submit_batch(app.on_input(inputs))
                                             # host-APP code, not engine: the app's own
                                             #   commands, on the host endpoint. No tick:
                                             #   the next drain takes them (§7.1)
        events = engine.drain_events()       # ALWAYS drained, bus or no bus (§7.3)
        if mods:                             # engine domain events -> per-mod inboxes;
            mods.publish(events)             #   non-blocking fan-out
        open_handles = app.on_outcomes(engine.outcomes(open_handles))
                                             # polls: `pending` until the command's tick
                                             #   has run; the app keeps those handles
        engine.render()                      # presents the latest published snapshot;
                                             #   blocks on vsync (GIL released)
                                             #   -> paces loop at 60
```

Rules:

- `app` in the sketch is the host application, not an engine object. Translating input into
  commands is application code. The sketch only shows *where* it crosses the boundary:
  `engine.submit_batch`, on the host endpoint.
- **The frame loop never waits on the engine; it polls.** Every call in the sketch except
  `render()` returns at once, and `render()` waits only for vsync. A full host endpoint
  returns `queue_full`. `outcomes()` reports `pending` for a command whose tick has not run
  yet, and the app reads it again on a later frame (§7.1). So a slow frame, a breakpoint or
  a dragged window delays only the frame. The sim keeps ticking, and nothing the loop calls
  can park the owner thread while the sim waits for it (`design_engine_core.md` §3.3).
- **The host names no tick.** The app submits on the host endpoint without one, and the
  first drain after the submit takes the commands (§7.1). Each outcome reports the tick its
  command ran in. The host is not a participant, so the sim never waits for the frame loop
  (`design_engine_core.md` §3.3).
- **There are two event streams, with different names.** `poll_input()` returns SDL input
  and window events. `drain_events()` empties the engine's own event ring (§7.3). They have
  different producers. The loop pumps both, and the mod bus fans out only the second.

  **Draining does not depend on having a mod bus.** The event ring is bounded, and letting
  it back up pauses the simulation (§7.3). A mod-less application that skipped the drain
  would stall its own session while doing nothing else wrong. A missing bus skips only the
  fan-out.
- **No mod commands cross here.** Mods submit on their own endpoints, from their own threads
  (§7.1). The loop's only mod duty is the fan-out, `mods.publish`. A relay would submit mod
  commands on the host application's endpoint. That would tie capacity, capability checks,
  attribution and revocation to the wrong principal. It would also put a paced producer's
  tick behind another thread's scheduling (§7.1).
- The sim thread reads the steady clock and takes whole ticks from the accumulator in C++.
  Python never computes `dt`. So Python timing jitter, GC pauses and scheduler noise can
  never make a run diverge from the recorded command stream, and transition determinism
  (`design_engine_core.md` §2) is untouched. They *can* shift when live commands are
  submitted and which tick drains them. That changes the live stream itself, which is
  allowed, because session reproducibility is not promised. The record captures whatever
  actually happened.
- Pacing of the frame loop comes from the vsync block inside `render()`, never from
  `time.sleep`.
- Sim rate and frame rate are independent, as timesteps and in execution. The accumulator
  separates the fixed step from the frame's `dt`, and 30 Hz ticks under a 60 fps render is
  the normal case. Ticks run on the sim thread while `render()` blocks on vsync.
- `render()` takes from its engine view once per frame. That is one atomic exchange, which also
  carries its return header back (`design_engine_core.md` §3.5). It then copies the chunks
  it needs, on the CPU, into the renderer-owned upload ring, which fences guard. GPU uploads
  and dispatches read that ring or cycled memory, never engine view memory (`design_engine_core.md`
  §3.1). If the snapshot tick has not changed since the previous frame, world state is
  identical. The frame still presents, because camera and UI are per-frame float state, but
  work derived from the tick may be cached or skipped.
- Stopping: a quit command or a window-close event sets the stop flag, and `request_stop()`
  sets it from code. The loop then returns, and `__exit__` runs the shutdown sequence (§2).
  A `KeyboardInterrupt` between pump calls unwinds through `__exit__` and releases cleanly.
- The loop borrows the mod bus and never owns it. Host lifetime belongs to the engine's set
  of registered dependants (§2). So leaving this function, by return, exception or
  `KeyboardInterrupt`, cannot leave mod hosts running against a closing engine. A loop that
  never receives a bus is no shutdown hazard. Whether the caller keeps a reference to `mods`
  is a question of convenience, not lifetime.

### 4.2 Pattern: headless and replay loops

Headless and replay runs advance with `step(n)` instead of the clock. This is the
verification harness of `design_engine_core.md` §2.3.

```python
# pseudo-code — the verification harness (design_engine_core.md §2.3)
with Engine(replace(config, headless=True)) as engine:
    engine.load_replay(path)                 # CONFIGURING: header (identities, versions,
                                             #   seed) + canonical command stream
                                             #   (design_engine_core.md §2.3)
    sim_estab.mod.load_mods(engine, policy)  # only what the header names; omit if none
    engine.start_session()                   # freeze: verifies identity vs the header
    engine.step(n_ticks)                     # exact ticks on this thread: no clock, no
                                             #   render, GIL released
    assert engine.checksum() == golden
```

- `step(n)` is the headless primitive. CI, golden replays and notebooks use it, so it must
  exist from the first build. It runs `n` ticks on the calling thread and returns the tick
  reached. The host is not paced, so a headless loop makes no per-tick call: commands the
  host submitted before `step(n)` run in its first tick.

- **A loaded replay puts the engine in `playback`, which closes every endpoint.** Every
  `submit*`, from every source including the host endpoint, returns `revoked`. `playback` is
  a mode, not an engine state. It is independent of §2's diagram: a replayed session goes
  through the same states as a live one (`configuring`, the freeze, `running`). The mode
  changes only submission and where the command stream comes from.

  The recorded stream is injected at consumption. It is already ordered and tick-stamped
  (§7.1), so submitting it again could be rejected differently. Without this mode, an
  ordinary `submit_batch` would add live commands to a replayed stream, and `checksum()`
  would diverge from `golden` for invisible reasons.
- **`load_replay` is a `configuring` call** (§2), and it must be. The header supplies the
  seed, which the freeze uses (`design_engine_core.md` §2.4 steps 4–6). Calling it while
  `running` raises `EngineStateError`: by then the seed is spent, and a log file already
  carries a different session's name.

  **So `load_replay` comes before `start_session()`.** In practice it also comes before
  `load_mods`, since the policy can name the mods the header requires only after the header
  has been read. Both calls leave the engine `configuring`. So the ordering constraint is
  between `load_replay` and the freeze, not between two loaders (`design_modding.md` §6). If
  a replay is loaded, `EngineConfig.seed` must be `None`. Supplying both is a `ValueError`
  at `load_replay`, not a silent precedence rule.
- **The header is a check, not a note.** At the freeze the computed identity is compared
  with the header, component by component. A mismatch raises `ReplayIdentityError` (§8),
  naming the first difference. So a different engine build, schema, content or mod set
  cannot silently produce "a replay that desyncs for invisible reasons". That is what the
  identity fields in the artifact are *for*.
- **Replay sessions load Tier 1 content and Tier 3 core mods.** Both are inside the
  determinism boundary and part of session identity, so the header names exactly which, and
  `load_mods` loads exactly those. **Tier 2 logic hosts are not spawned.** Their only effect
  is commands, which are already in the record. So a replay simply does not call
  `mods.start()`, and that is the whole mechanism.

  `ModPolicy.replay_observers` makes `mods.start()` spawn them anyway, as observers with
  their endpoints closed, for debugging (`design_modding.md` §3.1). That is a selection
  choice and invisible to the record, since the endpoints are closed either way. A replay
  that never spawns them pays nothing and registers no participant. That is also why a
  replay's gate has no participant at all.

### 4.3 Threading: the sim thread and `step(n)`

The API commits to the artifacts (commands in, snapshots and events out) and to the pump
call sites. Where ticks run follows from the kind of session:

| Session | Sim ticks | Render | The Python main loop does |
|---|---|---|---|
| windowed | on the C++ sim thread, started by `run_sim_async()` | inline in `render()` | submit → events → mod bus → outcomes → render |
| headless (CI, golden replays, notebooks) | in `step(n)`, on the calling thread | none | submit → `step(n)` → read |

- **Why a windowed session runs a sim thread.** A long sim tick cannot drop frames, and a
  slow frame cannot slow the sim. Fast-forward and pause become independent of render.
  Games that run a real-time simulation under a frame loop make the same split: Factorio,
  OpenTTD, Paradox's Clausewitz engine, Dwarf Fortress and Cities: Skylines all tick apart
  from the loop that presents frames. It costs nothing in the API, because the boundary
  mechanisms stay bounded and never block the sim (`design_engine_core.md` §3.2, §5.1). An
  engine view publishes with one exchange and cannot fail. A paced submit into a full ring
  waits for space, for at most one tick, and the sim never waits for it
  (`design_engine_core.md` §5.1). Whether that wait can happen at all is open
  ([Q8](open_question.md#q8-can-the-admission-wait-ever-happen)).
- **Why a headless session has none.** CI, golden replays and notebooks want exact ticks, not
  a clock. `step(n)` on the calling thread gives exactly `n` ticks, with no second thread to
  start, wait for or join. So a headless session has no sim thread, and `run_sim_async()`
  raises `EngineStateError` there.
- The sim thread is an implementation detail behind unchanged artifacts:
  - **Ownership.** The sim thread alone owns the steady clock, the accumulator, the tick
    loop and all core state. Every other thread touches only the three artifacts: commands,
    engine views and events. This is the same one-way boundary as the Python/C++ split, one level
    down. It holds because of *when*, not because of which threads exist: the sim thread
    owns core state while it runs. The two operations that read core state from the owner
    thread are the freeze's tick-0 publish and `checksum()`. Both happen at moments when the
    sim thread is provably not running a tick.
  - **One gate predicate, fed by independent fields.** The host is not a participant
    (`design_engine_core.md` §3.3). Its control state is:

    | Field | Meaning |
    |---|---|
    | `run_until` | exclusive tick ceiling set by pause, resume and step; `U64_MAX` means resumed |
    | `stop_requested` | sticky stop request; once set, it is never cleared |
    | `step_in_flight` | keeps step and resume apart; guarded by the gate mutex |

    The event backlog feeds the same predicate but is not a host field. It is a plain value
    owned by the sim thread, derived from the event ring's two indices
    (`design_engine_core.md` §5.2). It must not be a flag that both the sim and the draining
    owner thread write. Two writers deciding one fact from two counts, each read separately,
    can set it over an empty ring, and no recheck fixes that in both directions.

    Between ticks the sim evaluates one predicate: stop first, then the event backlog, then
    the finite or unlimited `run_until` grant. Pause, resume and step serialize their
    compound changes under the gate mutex, which every gate input changes under
    (`design_engine_core.md` §3.3). `request_stop()` is an independent store. So a concurrent resume cannot erase a stop, and draining the backlog
    cannot resume a sim the operator paused. **A backed-up event ring pauses the
    simulation.** It does not time the application out and does not end the session (§7.3).
  - **`step(n)` on a running sim thread is legal only while paused.** A paused sim thread
    runs zero ticks, so a manual step is not concurrent with anything. A `step()` while the
    sim thread runs unpaused raises `EngineStateError`. The rule exists to stop two things
    advancing the sim at once, not to forbid stepping a stopped clock.

    Pause-and-single-step is the ordinary way to inspect a live session. Without it,
    inspection would mean tearing down the sim thread and rebuilding it. That changes
    nothing about the sim and everything about what else is running. `resume()` while a
    `step()` is in flight is refused with `EngineStateError`, so the step cannot race a
    resume.
  - **`step(n)` has two executors, and whether a sim thread runs decides which one**, not an
    argument. With no sim thread (headless, replay, CI), the calling thread runs the ticks
    itself. With a sim thread, while paused, the sim thread runs them, and the owner thread
    only grants and waits. The caller never runs a tick behind the sim thread's back. Both
    return once tick `t+n` has published, and both return the tick reached.
  - **The return guarantee needs no bookkeeping of its own.** `step(n)` records `start =
    first_unexecuted` and sets the exclusive `run_until = start + n`, after checking for
    overflow. It then waits on the gate's tick-progress condition variable while
    `first_unexecuted < start + n` (`design_engine_core.md` §3.3). The sim stores
    `first_unexecuted` under the gate mutex after each tick's publish and notifies a waiter,
    so the wait follows the engine's one wake-up rule. The release/acquire pair orders the
    sim thread's tick and its publish before `step()` returns. That makes `checksum()` on
    the next line well-defined, instead of a race against the thread that owns core state.

    The waited-on condition is a counter that only increases, never a flag, so a waiter
    that wakes late still finds it true. And the notifications are never the condition: the
    waiter re-reads `first_unexecuted` every time it wakes, so a stray notification cannot
    make a step report ticks that never ran.
  - **Quiescence needs no separate rendezvous.** After `step(n)` returns, `run_until` equals
    `first_unexecuted`, so the gate blocks the sim thread by the same predicate that
    released it. The sim is provably parked. With no sim thread, nothing runs between calls
    at all. That is why `checksum()`, and any future state inspector, is legal on the owner
    thread at that point; §6 files them as owner-thread-only and quiescent. `pause()`
    remains a non-blocking control call. The gate, not the call, makes the sim stand still.
  - **The wait has no deadline, and Ctrl+C interrupts it.** The owner thread waits in short
    slices and checks for signals between them. A `KeyboardInterrupt` revokes the rest of
    the grant: under the gate mutex it sets `run_until = first_unexecuted`, so the sim
    parks at the gate after the tick it is running, and the exception propagates. A long
    tick is not a fault, and the host is not a participant whose lateness could be one. So
    nothing times the step out. A sim thread that never finishes its tick is caught at
    shutdown, by the staged join below. A `request_stop()` during a step returns the tick
    actually reached: stopping is not a failure.
  - **A stepped tick forces every due publication.** Cadence (§7.2) is a fast-forward
    optimization and never suppresses it. An engine view always has a writable block, so every due
    publication succeeds, and a stepped tick cannot silently drop one. `step()`'s return
    guarantee covers the completed core tick and its publication. A reader that must
    *observe* every stepped tick still registers a paced engine view. Publication guarantees the
    snapshot exists; pacing guarantees someone took it. Whether a stepped tick publishes
    engine views that are not due by their cadence is open
    ([Q12](open_question.md#q12-does-step-publish-every-engine-view-regardless-of-cadence)).
  - **The wait releases the GIL** (§5 rule 2). The owner thread may wait a long time, and
    every Python thread host would stall behind it. Releasing it between slices also lets
    the signal check run. On a GIL build, signal handlers run only on the main thread at
    bytecode boundaries, so a wait that held the GIL throughout would make
    `KeyboardInterrupt` undeliverable.
  - **Pause and time scale are control calls** (§3), never thread suspension. Pause sets
    `run_until = first_unexecuted`. The time scale is a separate loop-level atomic. The
    thread keeps looping while it runs zero ticks, or a scaled number. Neither is recorded:
    in tick time nothing happened, so replays are unaffected.
  - **Unbounded time scale** (`set_time_scale(None)`, §3). Ticks run back to back, with no
    wall-clock accumulator and no catch-up clamp: as fast as the core runs. Publication and
    event emission are unchanged, so render simply sees fewer of the states in between. The
    frame rate does not rise; the sim rate does. This is the windowed equivalent of headless
    `step(n)`. It gives a fast-forward button without touching `tick_rate`. The frame loop
    is not involved: the host declares nothing, so nothing it does per frame bounds the
    speed.
  - **"Unbounded" means unpaced, not free of every limit.** The event ring is sized `C × D`.
    `D` is the drain interval the ring is sized for: the ticks between two `drain_events()`
    calls (§7.1). A sim running free runs more ticks per drain, so a literally unbounded
    mode would overflow the ring by using a documented feature correctly.

    So the sim stops at the gate when the event backlog reaches `high_water`, and it resumes
    when the host drains. The backlog counts every entry the owner has not yet consumed.
    This is the ordinary backlog pause of `design_engine_core.md` §5.2, not a special case.
    Like a `pause()`, it has no deadline. A host that stops draining altogether pauses the
    simulation and is reported; it is never timed out into `failed` (`design_engine_core.md`
    §3.3). `D` therefore keeps its meaning in every session, and the ring's size still holds
    under fast-forward. The backlog is checked only at the gate, so whether the ring can
    fill within one tick is open
    ([Q10](open_question.md#q10-can-the-event-ring-fill-within-one-tick)). The value of
    `high_water` is open too ([Q63](open_question.md#q63-what-is-high_water)).
  - Fast-forward is also what makes per-engine-view cadence (§7.2) worth having. At unbounded speed
    most publishes are never looked at, and publishing every tick is O(projection) of pure
    waste. Lowering an engine view's cadence during fast-forward costs nothing observable, since
    every reader reads the latest publish either way.
  - **Stopping ends ticking.** `stop_sim_async()` ends ticking for the session, and the
    engine is `stopped` (§2). `stop_requested` is sticky, so a stopped sim thread is never
    restarted: `run_sim_async()` and `step()` raise `EngineStateError` once the session
    has stopped. `snapshot()`, `checksum()` and `close()` still work. To look at a live
    session, pause it and step it; to end it, stop it.
  - **Loop discipline.** Sleep to absolute deadlines, so the loop does not drift. Limit
    catch-up with the **catch-up clamp**, the most ticks one wake may run after falling
    behind. A stall then degrades into slow motion instead of a spiral of death, and a
    clamped wake emits a "sim behind" event. **The catch-up clamp bounds recovery from a
    stall, never a requested speed.** A `set_time_scale(8)` that the machine can sustain is
    not "behind", and must not be clamped per wake. Otherwise a deliberate fast-forward
    would silently cap itself and report itself as a problem. The clamp applies to the ticks
    owed that the accumulator reports. Unbounded mode has none, because it has no
    accumulator at all. The clamp's value is open
    ([Q21](open_question.md#q21-what-are-the-catch-up-clamp-and-idle_slice)).
  - **Every wait is named, and every wait on another party is bounded.** The core blocks in
    one place only, the gate. Every participant it waits for has a declared deadline and a
    declared expiry policy (`design_engine_core.md` §3.3). A pause and a backed-up event
    ring hold it with no deadline, because nothing in a paused session can be late. Callers
    have two more waits. Both are declared, and both are bounded by structure rather than by
    a timer: admission (at most one tick) and outcome (until the named tick runs), §7.1.
    Both belong to paced producers, mods and peers, on their own threads.

    **The owner thread waits for neither: the frame loop never waits on the engine; it
    polls** (`design_engine_core.md` §3.3). A full host endpoint returns `queue_full`, and
    `outcome(h)` returns `pending` until its tick has run. The owner thread waits in only
    two places, and only on the sim or on a deadline: `step(n)` (above) and the shutdown
    calls (§2). If a step's next tick is blocked by something only the owner can clear, the
    step returns or raises instead of waiting.

    That makes three waits on another party in the whole system. Anything else that waits
    is a defect, and so is any operation in this table that ever waits:

    | Operation | Why it never waits |
    |---|---|
    | publish | one `acq_rel` exchange, with no wait for the reader. It may commit pages of its reserved block before filling it; that is budgeted tick work, not part of the atomic step, and the block's address never changes (`design_engine_core.md` §3.1, §3.2) |
    | Engine view take | one `acq_rel` exchange, which also hands the reader's return header to the publisher (`design_engine_core.md` §3.5) |
    | the command drain | reads whatever index each producer has made visible and stops there. A command submitted a moment later is drained at the next tick (`design_engine_core.md` §5.1) |
    | `submit` into a ring with room | an operation lease, an endpoint lease, three bounded validity checks, then one release store. It never raises, and `queue_full` is a returned result (§6, §7.1). A *full* ring is the admission wait for a paced producer, which is declared above rather than absent here, and `queue_full` for the host |
    | `outcome(h)` on the owner thread | returns `pending` until the tick that drained the command has run (§7.1) |
    | `drain_events` | never blocks. *Not* calling it is what pauses the sim (§7.3) |

    The join is `stop_sim_async()`, and it is staged:

    1. set `stop_requested` under the gate mutex, and notify the sim if it is parked (the
       wake-up rule of `design_engine_core.md` §3.3). A sim thread at the gate then wakes
       and sees the stop;
    2. wait a generous first timeout, sized to cover a slow tick, since the flag is read
       only at the gate, between ticks;
    3. on expiry, log `critical` with the current tick and system name for attribution, then
       retry once;
    4. on success, re-raise any stored sim-thread error (§8);
    5. on the final timeout, the engine enters the terminal `failed` state (§2). This call
       then raises `EngineFailedError`, or hard-aborts, depending on `on_failed_stop` (§3).

    `close()` makes the same attempt but never raises. A failed join leaves the engine
    `failed`, with resources leaked on purpose, never freed under a live thread. **The
    staged join releases the GIL** (§5 rule 2). Holding it would freeze every thread host
    for the join window. That is safe only because shutdown step 1 has already stopped the
    mod hosts (§2).
- Render stays on the Python main loop, as checked against SDL3: **window = event pump =
  swapchain present = Python main thread.** SDL3 GPU forbids swapchain acquisition off the
  window's thread, and there are no plans to lift that restriction.
- Known limit: a stall on the Python main thread (GC, or a rogue mod on the main thread) can
  delay the *presentation* of an otherwise finished frame. It never holds the sim, because
  the host is not a participant. Full render independence would need a raw-Vulkan viz
  backend, which can present from any thread. That stays a possible future backend swap
  behind the C++ viz interface, with no change to the Python API. It is not planned until
  frame-pacing measurements demand it.
- Window lifecycle (create, destroy, resize handling) always stays on the main thread.

## 5. Boundary and performance rules

**TL;DR**: five rules keep the Python loop free. Most limit what crosses the Python/C++
boundary, or how long a crossing holds the GIL. Rule 5 sets which thread may make each call.

1. **O(1) boundary crossings per frame.** Never O(ticks), never O(entities). Commands cross
   in batches, snapshots as one handle, and events as one drained batch. This bounds the
   *loop's* crossings, which is why no mod command crosses in it (§7.1). A large mod batch
   costs that mod's thread, never the frame.
2. **Every native call that may run long releases the GIL**
   (`nb::call_guard<nb::gil_scoped_release>` on `step`, `render` and replay operations). On GIL builds this keeps other Python threads, such as mod hosts, running.
   On free-threaded builds it detaches the thread state, so long native work never blocks
   stop-the-world GC. The same annotation is correct on both. **Blocking joins are
   included.** The staged join under `stop_sim_async()` and `close()` is the longest
   blocking call in the API, and the one where holding the GIL costs most (§4.3). Mind the
   granularity: the guard goes on the *native* join binding. It never goes on the
   Python-level `close()`, whose step 1 runs mod `on_unload` and so needs the GIL (§2).
3. **No Python callbacks at tick rate.** There is no `on_tick` for Python code, host or mod.
   Frame-rate readers read snapshots and events, and the world changes only through
   commands. Whether every mod must still declare ready every tick is open
   ([Q2](open_question.md#q2-must-every-mod-be-paced-every-tick)).
4. **Per-frame snapshot access is zero-copy** (§7.2), through a registered `PRIVATE` engine view.
   Copying a full projection every frame would dwarf every other cost in this document. That
   is why render owns an engine view instead of calling `engine.snapshot()`. Occasional readers
   copy, and that is the right default for them.
5. **Thread contract** (§6). The `Engine`'s pump and lifecycle calls are externally
   synchronized. `submit*` is single-producer per endpoint. `view.take()` belongs to the
   engine view's single reader. Array views are freely shared. Whether `snapshot()` may be called
   from any thread is open
   ([Q3](open_question.md#q3-can-enginesnapshot-be-called-from-any-thread)).

## 6. Normal + free-threaded Python

**TL;DR**

- One codebase serves the GIL build (`cp314`) and the free-threaded build (`cp314t`).
- Correctness comes from the thread contract and C++-side mutexes, not from binding-level
  locks.

Rules:

- `nanobind_add_module(... FREE_THREADED ...)` stays; it is already set in
  `src/sim_estab_ext/CMakeLists.txt`. Without it, a free-threaded interpreter silently
  re-enables the GIL at import. A test guards it:

  ```python
  # pseudo-code — test_freethreading.py
  if sysconfig.get_config_var("Py_GIL_DISABLED"):
      assert not sys._is_gil_enabled()      # would fail if FREE_THREADED regressed
  ```

- **Every native API that can overlap `close()` holds an operation lease.** The lease counts
  running calls. It is not a per-call mutex: admitted calls stay concurrent, as the API
  families below allow. The control block outlives the native resources and uses this
  protocol:

  ```text
  enter:
      if access_state.load(acquire) != OPEN: raise EngineClosedError/EngineFailedError
      active_operations.fetch_add(1, acquire)
      if access_state.load(acquire) != OPEN:
          active_operations.fetch_sub(1, release); notify_if_last()
          raise EngineClosedError/EngineFailedError
      # the second OPEN observation is the call's admission linearization point

  leave:
      active_operations.fetch_sub(1, release); notify_if_last()

  close:
      compare_exchange access_state OPEN -> CLOSING (acq_rel)  # close LP
      wait_until(shutdown_deadline, active_operations == 0)
      release native resources; access_state.store(CLOSED, release)
  ```

  The increment followed by a recheck closes the gap between check and use. Either the call
  is counted before `close()` waits, or it sees `CLOSING` and never dereferences native
  state. The close side checks its predicate under its cold-path mutex, and the last
  decrement notifies while closing. A timeout of the wait stores `FAILED`, disarms every
  native context, and logs the resources it keeps. Every admitted path installs a scope
  guard before it touches native state, so exceptions and early returns run `leave`.
  `close()` itself takes no lease.

- The thread contract, per API family:
  - **Lifecycle and pump** (`Engine` constructor, `start_session`, `load_replay`,
    `load_content`, `load_native_mod`, `register_hosts`, `close`, `step`, `render`,
    `poll_input`, `drain_events`, `run_*_async`): externally synchronized, with exactly one
    driving thread, the owner thread of §2. An always-on check raises
    `EngineThreadError` (§2). The C++ layer below keeps its abort-on-invariant behaviour.
    `close()` is in this family but never raises; how the two combine is open
    ([Q13](open_question.md#q13-can-close-be-called-from-another-thread)).
  - `drain_events()` is in this family because it empties a single queue. Two callers would
    each get a disjoint half, and each would conclude the other half never happened. The
    owner thread drains, and the mod bus fans out (§4.1).
  - **`submit` and `submit_batch`**: callable from any thread, on any build, but **one
    thread at a time per endpoint**, because the ring is single-producer by contract (§7.1).
    Two threads sharing one endpoint is a usage error. Two threads on two endpoints never
    interact. Each call holds the operation lease above, and the endpoint lease of
    `design_engine_core.md` §5.1, until it has finished touching the ring.
  - **The host endpoint's producer is the owner thread.** `engine.submit`,
    `engine.submit_batch` and the host's `outcome` calls belong to it. None of them parks:
    a full host endpoint returns `queue_full`, and `outcome(h)` returns `pending` until its
    tick has run (§4.3).
  - **`engine.snapshot()`**: specified as thread-safe from any thread, on any build. It
    copies out of the default engine view under the owner thread's take, and touches no per-caller
    state. Its operation lease encloses the copy. This family matters because mods call
    `ctx.snapshot()` from their own mod host threads (`design_modding.md` §4.1). Filing it
    under "lifecycle and pump, externally synchronized" would make every mod snapshot a
    contract violation. The default engine view is `PRIVATE`, with one reader, so whether other
    threads can really call it is open
    ([Q3](open_question.md#q3-can-enginesnapshot-be-called-from-any-thread)).
  - **`view.take()`**: only the `PRIVATE` engine view's single reader thread, whichever thread that
    is. A `PRIVATE` engine view has one reader by definition (`design_engine_core.md` §3.2). Two
    threads taking from one engine view is the same usage error as two producers on one endpoint.
  - **Array views and drained events**: immutable, and freely shared across threads without
    locks. Sharing an array view keeps its block alive, and so delays the next `take()`
    (§7.2). That is a lifetime consequence, not a thread-safety one.
  - **`checksum()`**: owner thread and quiescent. It is its own family, and the only one
    with a second condition. It reads core state, which the sim thread alone owns (§4.3). So
    it is legal only while that thread stands still: no sim thread runs, or it is paused
    with no step in flight, or the session has stopped. The call confirms quiescence itself (§4.3) instead of
    making the caller arrange it, so `step(1); checksum()` is correct as written. Calling it
    while the sim runs raises `EngineStateError`. Filing it anywhere else would make the
    verification harness's own idiom a contract violation.
  - **Control calls** (`pause`, `resume`, `set_time_scale`, `request_stop`,
    `stop_requested`, `set_log_level`): thread-safe from any thread, on any build.
    - `pause`, `resume` and `step` serialize their compound grant changes under the gate
      mutex. `request_stop` sets the independent sticky stop atomic, under the same mutex.
      All of them notify a parked sim (`design_engine_core.md` §3.3).
    - The event backlog is a plain value owned by the sim thread (`design_engine_core.md`
      §5.2), so no control call writes it. How a drain wakes a sim parked on the backlog is
      open ([Q11](open_question.md#q11-how-does-draining-events-wake-the-sim)).
    - `set_time_scale` uses its loop-level atomic. `set_log_level` calls a subsystem with
      its own mutex (`design_logging.md` §2).

    These calls are not owner-only, on purpose. `request_stop` from a watchdog thread and
    `pause` from a debug console are both ordinary uses, and making them safe costs nothing.
    `resume()` still raises if a `step()` is in flight (§4.3). It decides this while holding
    the same gate mutex that creates the grant.
  - **`raise_if_failed()`**: owner thread only. It is one of the async-error rendezvous
    points (§8), and those are defined on the driving thread.
- **Release timing follows scope, not reference counts.** Do not rely on rebinding or `del`
  to release an engine view block promptly. On free-threaded builds, deferred and biased reference
  counting make "the old handle dies when it is rebound" a timing assumption, not a
  guarantee. Hold snapshots in `with` blocks, and bound the derived array views in a
  function frame (§7.2). That is exact on both builds. A design that needs prompt release
  without `with` behaves differently on `cp314` and `cp314t`.
- Do not add per-call binding locks (`nb::arg().lock()` and similar) to make the pump
  "safe". Paying for a lock every frame to hide a usage error is the wrong trade. The
  operation lease is an atomic lifetime guard, not mutual exclusion. The gate mutex guards
  only the gate's inputs, and nothing holds it across a wait.
- On GIL builds, Python mod threads take turns with the main loop. On free-threaded builds
  they run truly in parallel. `design_modding.md` §4 handles the consequence for mod
  hosting: heavy or untrusted mods go to a process host.
- Distribution: `cp314` and `cp314t` are distinct ABI tags, and the stable ABI does not
  cover free-threaded builds. Plan a wheel matrix per interpreter. `STABLE_ABI` stays
  commented out in CMake.
- GC pauses are the only Python-side threat to flat frame times. The mitigation, applied
  only if the frame-time histogram shows a need: `gc.freeze()` after startup and mod load,
  and tuned thresholds.

## 7. The boundary artifacts

Everything outside the engine sees exactly three artifacts: commands, snapshots and events.
That includes user code, logic mods and future network peers. This section gives their
Python bindings.

### 7.1 Commands (inbound, the only write path)

Commands are the only way to change the world. Every command gets two answers. A paced
command names its tick and runs at that tick or not at all. A host command names no tick
and runs at the first tick that drains it.

```python
# pseudo-code
cmd = commands.spawn_unit(pos=(q, r), owner=player_id)   # typed builder -> packed struct
h   = engine.submit(cmd)                # host endpoint; ADMISSION. No tick: the next
                                        #   drain takes it
hs  = engine.submit_batch(cmds)         # one crossing (§5 rule 1); one handle per command,
                                        #   in array order
...
res = engine.outcome(h)                 # never waits: `pending` until the tick that drained
                                        #   it has run, then what it DID and at which tick

# a paced producer (a mod, design_modding.md §4.1) names its tick and releases it
h   = ctx.submit(cmd, t)                # ADMISSION for the named tick
ctx.declare_ready(t)                    # releases t
res = ctx.outcome(h)                    # waits until t has run; t is already released
```

**A paced submission names its tick.** There is no unstamped paced call and no peer-only
variant. A mod and a network peer use the same function. What differs is one number their
endpoint declared at the freeze, its **stamp margin**: 0 for a mod, or a peer's input delay
(`design_engine_core.md` §5.1). Nothing returns a lower bound and then chooses, because a
lower bound is the one answer a submitter cannot act on.

**Every paced producer is a participant.** Submitting for a named tick makes a caller a
participant (`design_engine_core.md` §3.3): the tick does not advance until every source
that acts in it has had its say. So waiting is how this API works for mods and peers, not a
blocking mode with a non-blocking alternative. A producer that named a tick the engine did
not wait for would be a producer whose commands can be dropped.

**The host is the one unpaced producer.** In single-player its commands name no tick, so
none of them can be late, and nothing has to wait for the host. The frame loop never waits
on the engine; it polls (§4.3). In a networked session the local player submits through its
own peer endpoint instead (`design_multiplayer.md` §3.2).

**One endpoint per producer, and an endpoint is single-producer.** A command is submitted by
the call that submits it, on the calling thread, through an endpoint bound to its
**source**:

| Principal | Endpoint | Margin | Call |
|---|---|---|---|
| host application (player input, tools) | the **host endpoint**, source id 0. Allocated at the freeze, opened when the session starts, revoked at shutdown after its running submits finish. Its producer is the owner thread (§6) | none: unpaced. Its commands name no tick, and the first drain after a submit takes them | `engine.submit` / `engine.submit_batch` |
| logic mod (Tier 2) | the mod's stable endpoint, allocated at the freeze and bound to its assigned source id. Opened at `mods.start()` or on retry; revoked at shutdown step 1 after its running submits finish (`design_modding.md` §6) | 0 | `ctx.submit` / `ctx.submit_batch` |
| network peer | one endpoint per peer, bound at the freeze, with its own receive thread | the input delay | the transport's `submit` |

Single-producer is a contract, and it is what allows the SPSC ring (`design_engine_core.md`
§5.1). A caller with several producer threads registers one endpoint per thread at the
freeze, or serializes its own submissions. How the drain orders two endpoints of one source
is open ([Q62](open_question.md#q62-in-what-order-are-two-endpoints-of-one-source-drained)).
Concurrent `submit` on one endpoint is a usage error, diagnosed in debug builds and
undefined in release. It is the same class of contract as "one owner for `drain_events()`"
(§6). There is no relaying. Mod commands never travel through the event bus, and are never
submitted again on the host endpoint (`design_modding.md` §4.1).

Rules:

- **Two answers per command, at different times.** Submitting answers *is it in the queue?*
  The outcome answers *what happened when it ran?*, and that answer does not exist until the
  tick runs. Neither answer is an event, and neither can be lost.

  | Aspect | **Admission**: the return of `submit` | **Outcome**: read from the handle |
  |---|---|---|
  | Who | every producer, peers included | every producer |
  | Why | the alternative is dropping a command, and for a peer a dropped command is a certain desync | feedback, and the only place a rejection at application time is reported |
  | Length | a paced producer: until space frees, at most one tick. The host: none; a full ring returns `queue_full` | a paced producer: until the named tick has run. The host: none; `pending` until its tick has run |
  | Can it fail? | only if the producer breaks its own contract (the table below) | no. It *reports* failures; it is not one |

- **For a paced producer, admission is a wait, not a coin flip.** `submit` blocks until the
  ring has room. The wait is bounded by one tick, because the sim frees a tick's worth of
  entries every tick. It cannot deadlock. An endpoint's capacity is by definition at least one tick's worth of
  its own commands, so a producer inside its own allocation always fits. Whether the wait
  can happen at all is open ([Q8](open_question.md#q8-can-the-admission-wait-ever-happen)).
  A pause wakes every waiter with `session_paused`, which means *retry*, never *failed*
  (`design_engine_core.md` §3.3). Whether a caller ever sees `session_paused` is open
  ([Q7](open_question.md#q7-does-a-caller-ever-see-session_paused)).

- **The deadlock rule, and the engine enforces it.** A deadlock exists only if something
  waits for the outcome of a tick it is itself holding up. Ordering the calls removes it:

  ```
  submit(cmd, t)        # admission
  declare_ready(t)      # releases the tick
  ...engine drains, runs, advances...
  outcome(h)            # a tick already released
  ```

  **You may only wait for outcomes of ticks you have already released.** The engine knows
  who is paced and which tick each one holds. So a wait that breaks the rule raises
  `CommandOrderError` (§8) at once, naming the tick. The result is a loud error at the call
  site, not a frozen session. The rule binds paced producers only. The host's `outcome(h)`
  never waits, so it has nothing to deadlock on.

- **One capacity rejection, and only one.** `queue_full` means *more than you allocated for
  this tick*: a producer breaking its own declaration, not a busy engine. An engine that is
  briefly behind makes a producer wait; it never rejects. So the full result set is the
  contract violations, one lifecycle state and one transport failure. None of them is a load
  signal:

  | Result | Class | Meaning |
  |---|---|---|
  | `admitted{handle}` | none | in the ring for the tick you named; the handle reads the outcome |
  | `queue_full` | contract | more than `capacity` commands stamped for one tick on this endpoint. On the host endpoint: more than `capacity` commands waiting for the next drain |
  | `too_late` | contract | the named tick is strictly in the past |
  | `out_of_order` | contract | the named tick is earlier than one already named on this endpoint. Stamps never decrease, and this result enforces it |
  | `over_margin` | contract | further ahead than this endpoint's declared margin; for a margin-0 producer, any tick but the current one |
  | `invalid` | contract | a malformed payload, or a capability the source does not hold |
  | `revoked` | lifecycle | this endpoint is not admitting: it is torn down, or the session is in playback (§4.2). Terminal for the caller |
  | `host_error` | transport | the submitter's own transport failed or timed out; process hosts only (`design_modding.md` §4.3) |

  The host endpoint names no tick, so it never returns `too_late`, `out_of_order` or
  `over_margin`.

  Results are returned, never delivered, and they never raise. A return value has no size
  limit, no second recipient and no overflow policy. So an outcome cannot be dropped, split
  between callers, or used to flood a queue.

- **`too_late` is `t < first_unexecuted`: strictly past, and nothing else.** The tick the
  engine is currently waiting on is not late. A command stamped for it is the normal case,
  since the producer submitting is the one the gate is waiting for. In a healthy session
  `too_late` cannot happen, so it reports a defect, not load.

- **Revocation closes the endpoint before reclaiming the ring.** Every submit takes the
  endpoint lease of `design_engine_core.md` §5.1, then touches the ring. Revocation changes
  `OPEN -> REVOKING` and refuses new leases. It wakes any producer parked in the admission
  wait with `revoked`. It then waits, under the shutdown deadline, for calls already
  admitted, and stores `REVOKED`. A timeout of that wait is a `failed` path (§2).

  A call admitted before revocation may still return any ordinary result. A later poll of
  `ctx.stopping` is only advisory. The submit result is authoritative, so seeing `stopping
  == False` and then `revoked` is a legal race, not a contradiction.

- **Outcomes are never events, and rejections are outcomes.** The line is drawn by *who is
  still there to be told*:

  | Class | Cause | Channel | Recorded |
  |---|---|---|---|
  | submission time | a capability, capacity, a bad stamp, a revoked endpoint, a malformed payload | the return of `submit` | never |
  | application time | the named entity is gone at consumption; application rules reject it; a declared game-rule cap refuses a commanded create (`design_data_container.md` §2.2) | the outcome, read from the handle | reproduced, not stored |

  Both are answers to a caller that still exists. That is why neither is an event, and why
  the event ring never has to be sized for per-command rejections. What does reach the event
  bus is the case with no caller behind it: a system's create refused by a game-rule cap,
  reported once per `(type, source, tick)` (§7.3, `design_data_container.md` §7.3).

  "Reproduced, not stored" means this. The artifact holds the command, and replaying it
  re-derives the rejection, because the outcome is a deterministic function of the consumed
  command and the state at tick N. Nothing about the rejection is written to the replay
  file.

- **Capacity is the only number an endpoint declares.** `capacity` is how many commands it
  may hold for one tick. There is no drain quota: a tick runs everything stamped for it,
  because pacing closes the set before the drain runs (`design_engine_core.md` §5.1). Ring
  depth follows, `(margin + 1) × capacity`, and so does everything else:

  | Quantity | Bound |
  |---|---|
  | endpoint ring depth | `(margin + 1) × capacity`: computed, never configured |
  | engine-wide commands per tick, `C` | `Σ capacity(endpoint)` over *registered* endpoints: a bound checked at the freeze, not an allocation |
  | reliable-class event volume to the host | `≤ C` per tick |
  | event ring size | `≥ C × D`, where `D` is the drain interval: the ticks between two `drain_events()` calls that the ring is sized for |

  This sizing assumes events are bounded by commands. Events the engine creates itself are
  not, and what that means for the ring is open
  ([Q10](open_question.md#q10-can-the-event-ring-fill-within-one-tick)).

  `D` is not the catch-up clamp (§4.3). The clamp bounds ticks per *wake*, and a host
  stalled across several frames spans several wakes. `D` is the drain interval the event
  ring is sized for: a tolerance, not a promise (`design_limits.md` §2). An application that
  exceeds it pauses the simulation (§7.3) instead of overflowing the ring.

  Endpoint capacity is decided in `design_limits.md` §2, which carries the reasoning and the
  revisit trigger. It lives in `EngineConfig.command_policy` (§3). Ring depth, `C` and the
  event ring size are **computed at the freeze, and are neither fields nor properties of
  `CommandPolicy`**. The endpoint set and the stamp margins are not known before the freeze.
  An implementation that writes any of them down has added a second source of truth.

- **Order comes from the drain, and needs no sort.** `(source id, sequence)` is a total
  order by construction. The drain visits endpoints in ascending source id and takes each
  ring in FIFO order, assigning `sequence` by position. No source-supplied field can be
  forged, and no tie is resolved by arrival timing. The iteration-order class of desync
  (`design_engine_core.md` §2.2) cannot occur at all, rather than being sorted away.
- **`submit_batch` is atomic and consecutively sequenced, at no extra cost.** An endpoint is
  single-producer, so `k` submits from it are always contiguous in its ring. They never
  interleave with another thread's commands from the same source. Ordering the array orders
  the commands.

  A batch larger than the endpoint's per-tick capacity cannot succeed whole. The size bound
  is derived from capacity, not picked. A batch that exceeds it is admitted as far as it
  fits, with `queue_full` for the rest, in array order. Partial acceptance is the honest
  outcome. The alternative would be to reserve the whole ring before publishing anything,
  which needs a staging arena, and this design has none. Nothing bounds *attempts*. A
  producer may retry freely, and pays in its own mod host CPU accounting
  (`design_modding.md` §4.2), never in shared capacity.
- **Playback closes every endpoint** (§4.2). While a replay is loaded, no endpoint admits
  and every `submit` returns `revoked`. The recorded stream is injected at consumption
  instead.
- Commands are validated against the submitter's source-id capabilities (`design_modding.md`
  §3).
- **Recording**: the input stream is recorded at tick consumption, so it holds exactly the
  commands the core runs. The recorded stream is therefore identical to the applied stream
  by construction. Rejected commands never enter the record. No command can be admitted but
  unrecorded, or recorded but dropped, and so corrupt a replay.
- **One schema, three uses.** One canonical command payload schema serves the replay file,
  the mod-host IPC wire and the Python builders. It uses explicit little-endian, fixed
  widths and full validation (`design_engine_core.md` §2.3). Never define a second payload
  encoding *for commands*. Transport *envelopes* differ by use: replay chunks carry indexes
  and checksums, and IPC frames carry length framing and the engine-assigned source
  endpoint. Native structs are never memcpy'd across either boundary.

  **Events have their own payload schema, under the same encoding rules, with two uses, not
  three**: the IPC wire and the Python binding. They are absent from the replay artifact by
  construction. That artifact is a command stream, and a replayed tick re-derives its events
  by running the commands (§7.3, `design_modding.md` §4.3).
- Builders are generated from, or checked against, the C++ command registry, so Python and
  C++ cannot drift.

### 7.2 Snapshots (outbound state)

State leaves the core through an engine view (`design_engine_core.md` §3.1). Python sees two
surfaces over it, and which one you use decides which cost you pay. Every engine view is `PRIVATE`
in v1.

```python
# pseudo-code

# 1. The default: a copy out of the engine's default engine view. Any number, any thread,
#    outlives everything. No registration, no lifetime rules, no refusals.
snap = engine.snapshot()                     # process-owned copy
print(snap.tick, snap.column("pop.position")[0])

# 2. Opt-in zero-copy: an engine view, registered before the freeze.
engine.register_view("ui", columns=["pop.id", "pop.position"], cadence=1)
...
view = engine.view("ui")                     # the handle; one owner, for the session

def sample(view):
    with view.take() as snap:                # one atomic exchange; zero-copy
        pos = snap.column("pop.position")    # read-only ndarray view, NO COPY
        ids = snap.column("pop.id")          # uint64 identity, ascending
        return snap.tick, ids[42], float(pos[0][0])
    # `with` drops the handle; `pos`/`ids` die with the function frame.

tick, watched, x = sample(view)              # only scalars escaped

# 3. Row filtering: declare the predicate KIND at registration, steer its
#    PARAMETERS per frame. The parameters ride the same exchange as the take.
engine.register_view("render", columns=["pop.position", "pop.kind"],
                     cadence=1, predicate="sphere")
rv = engine.view("render")
rv.set_predicate(centre=cam.pos, radius=cam.far * 1.5)   # conservative, on purpose
with rv.take() as snap:                      # publishes the params, takes the block
    draw(exact_frustum_cull(snap, cam))      # refine here, with a FRESH camera
print(rv.lag)                                # ticks behind; 0 means keeping up
```

Rules:

- **`engine.snapshot()` returns a copy.** It reads the default engine view: the full `[[=viz]]`
  projection at cadence 1, with predicate `ALL`. That engine view is `PRIVATE`, and its reader is
  the owner thread. `snapshot()` copies out of the block that thread holds, then returns
  process-owned memory. Its operation lease encloses the copy (§6), so shutdown cannot free
  the engine view under it.

  Because it copies instead of retaining, the owner thread's block is free again when the
  call returns. So the default engine view never needs a retention rule, and no caller can cause a
  refusal in another. It does not interact with any other reader and never returns a torn
  snapshot. Most code should use this call. Making it the default keeps the ordinary
  retained value free of engine lifetime rules. Its cost at large world sizes is open
  ([Q4](open_question.md#q4-can-the-default-engine-view-afford-to-publish-everything-every-tick)),
  and so is calling it from threads other than the owner thread
  ([Q3](open_question.md#q3-can-enginesnapshot-be-called-from-any-thread)).
- **Zero-copy needs a registered `PRIVATE` engine view.** Zero-copy means holding engine memory,
  and only a single-reader engine view can promise that the publisher will not write it
  (`design_engine_core.md` §3.2). Registration happens before the freeze
  (`design_engine_core.md` §2.4 step 3a). It declares the column subset and the cadence, so
  the cost the engine view adds to every publish is visible where it is chosen.

  There is no duplicate `EngineConfig.views`. Host-application engine views are registered by calls
  during `configuring`. Mod engine views are declared in manifests, and the loader registers them
  before the freeze (`design_modding.md` §6). Both paths end in the same closed registry.
- **An engine view holds exactly one snapshot at a time, for as long as the reader likes.** `take()`
  returns the newest published block and hands the previous one back. There is no budget, no
  quota and no starvation. The publisher always has a block, and a reader that never takes
  again simply keeps looking at an old tick.
- **`take()` is refused while array views on the previous snapshot are alive.** This is the
  one sharp edge:

  ```python
  with view.take() as snap:
      pos = snap.column("pop.position")

  pos          # still alive -> the previous block is still yours
  view.take()  # raises ViewBusyError
  del pos
  view.take()  # fine
  ```

  The block handed back is the next one the publisher writes. A take while a derived array
  view is alive would silently invalidate that view. **Nothing is ever released without its
  holder's knowledge**, so the take is refused instead. Three things follow:
  - The refusal is caused by your own retained array views and by nothing else. No other
    reader, mod or thread can cause it, and no other reader can be affected by yours. A
    shared quota could never promise that.
  - `with` releases the *handle* deterministically on both builds, but **`with` alone is not
    enough**. Python has no block scope, so an array view bound inside the block is still
    alive after it. Bound the array views too. A function frame is the idiomatic way, as the
    example does. Advice that says "just use `with`" is incomplete, and produces
    `ViewBusyError` in the very code that follows it.
  - `__exit__` does not revoke array views, and must not. Array views stay valid when shared
    with other threads because scope exit cannot pull memory out from under them.
- **Retention and archiving use a copy**: `engine.snapshot()`, or `snap.copy()` inside a
  `take()`. Copies may exist in any number and outlive everything, including `close()`. The
  copy is the pressure valve that makes the one-block rule workable. It is available
  wherever a snapshot is, including `ModContext` (`design_modding.md` §4.1).
- **Track ids, never snapshot rows.** A row of a tick-N snapshot means nothing at N+k:
  publish gathers only live rows, so an erase or create shifts every later row
  (`design_data_container.md` §5.1). The `id` column is present in every projection, so
  re-resolving is always possible. Whether it really is in every projection is open
  ([Q5](open_question.md#q5-is-the-id-column-in-every-projection)). One rule holds for every
  object type: snapshot rows are in slot order, and an id's high half is its slot, so the
  `id` column is sorted ascending (`design_data_container.md` §2.2). `np.searchsorted` is
  therefore correct everywhere, and `snap.find` is the convenience built on it:

  ```python
  def still_alive(view, watched):
      with view.take() as snap:
          return snap.find("pop", watched) is not None   # searchsorted, then an equality check
  ```

  `snap.find` returns the row whose id equals `watched`, or `None`. The equality check
  matters: an id whose entity was erased has the same slot as any later occupant, but a
  different generation, so a search for it finds a neighbour and the check rejects it.
- **Cadence is per engine view**, declared at registration. There is no global snapshot-cadence
  control call. An analytics engine view at `cadence=30` costs a thirtieth of a render engine view. A tick
  under a `step()` grant attempts every engine view regardless of cadence, and every attempt
  succeeds (§4.3). Whether `step()` really publishes engine views that are not due is open
  ([Q12](open_question.md#q12-does-step-publish-every-engine-view-regardless-of-cadence)).
- **The predicate kind is declared at registration; its parameters are set at run time.**
  `predicate=` takes one of `"all"` (the default), `"aabb"`, `"sphere"`, `"frustum"` or
  `"tag"` (`design_engine_core.md` §3.4). `view.set_predicate(**params)` writes the
  parameters into the engine view's return header. They reach the publisher at the **next
  `take()`**, because that exchange is what publishes them (`design_engine_core.md` §3.5).
  Two consequences follow:
  - **Parameters are one publish cycle stale, by design.** The core's predicate is
    conservative on purpose: a region, not a visibility answer. So the staleness needs no
    safety margin. Exact culling belongs to the reader, against the camera it has this
    frame.
  - **`set_predicate` without a later `take()` does nothing.** It is not a control call and
    never enters the command ring. It can never affect the simulation, only which rows this
    engine view contains.
- **A filtered engine view projects `pop.id` implicitly.** Row 3 of a filtered snapshot is not
  entity 3. So any predicate other than `"all"` adds the `id` column, at 8 bytes per matched
  row, whether or not it was requested. A renderer that only draws may declare
  `identity=False` to decline it. GPU state must not key on a snapshot row across frames
  anyway (`design_data_container.md` §5). Per-entity GPU state would key on slot and
  generation instead, and how an engine view projects them is open
  ([Q57](open_question.md#q57-gpu-per-entity-state-across-frames)). How `identity=False` fits with the `id` column being in
  every projection is open ([Q5](open_question.md#q5-is-the-id-column-in-every-projection)).
- **`view.lag` and `view.matched_rows`** are the two counters the return header provides.
  `lag` is how many ticks behind the reader is, computed from the `last_consumed_tick` that
  the return header carries back. A single-reader engine view never skips a publish, so without
  `lag` a lagging reader is invisible. `view.hint_cadence(k)` is the advisory back-pressure
  that goes with it. The publisher may ignore it, and a reader that cannot miss a tick uses
  `paced=True` instead.
- **An engine view may be paced.** Registering it with `paced=True` and a deadline makes its reader
  a participant (`design_engine_core.md` §3.3). The core will not advance past a tick the
  engine view has not taken. That is how a recorder or training-data collector makes sure it misses
  no tick. It costs what it says: up to the declared deadline of tick latency, and only
  while that engine view is behind. How a take counts as readiness is not yet specified
  ([Q14](open_question.md#q14-how-does-a-paced-engine-view-count-as-ready)).

  The default is `False`, and an engine view that is not paced cannot delay a tick, however slow it
  is. Pacing is the only way to guarantee no missed tick. A reader that only wants the core
  to ease off sends a cadence hint in its return header instead (`design_engine_core.md`
  §3.5). The hint is advisory and never enters the gate.
- **`close()` waits for running calls, then detaches retained blocks. It never raises, and
  never frees memory under a live array view** (§2). The wait for running calls is bounded
  by the shutdown deadline. Live array views are a separate matter: they can be neither
  waited out nor revoked.

  Three guarantees would otherwise conflict: `close()` never raises, `close()` is bounded,
  and zero-copy array views are freely shareable. `nb::ndarray` hands a raw pointer to
  NumPy, and a NumPy array can be read without re-entering our code. So revocation cannot be
  enforced. The resolution is **detach**:

  | At close | Outcome |
  |---|---|
  | an in-progress `snapshot()` or other native API call | admitted before `CLOSING`; allowed to finish under the shutdown deadline |
  | a block held by a live array view | detached from its engine view, and freed when the last array view on it is dropped |
  | Engine views, rings, contexts, devices | released normally |
  | the handle | raises `EngineClosedError` on anything that re-enters the engine |
  | the array memory | stays valid and frozen. Nothing observable changes, since it was immutable anyway |

  If the wait for running calls misses its deadline, none of the native rows above is
  released. The engine enters `failed`, disarms its contexts and keeps the whole resource
  graph. Detaching is allowed only after the count reaches zero.

  - **Detach happens only at close, never on the publish path.** Publishing writes a block
    the engine view already owns. A detach there would force the publisher to allocate a
    replacement outside the tick budget, which `design_engine_core.md` §3.2 forbids. At
    close, publishing has already stopped, so that allocation never happens. A block's
    address never changes: it reserves address space for the engine view's maximum rows and commits
    pages as matched rows reach them (`design_engine_core.md` §3.1).
  - Remaining cost, stated so it is not discovered later: a caller that parks an array view
    in a global keeps that memory until interpreter exit. The cost is bounded: one block per
    `PRIVATE` engine view, once per process, since one engine is closed once and an engine view holds one
    block. It can neither recur nor grow.
- Fixed-point to float conversion happened at publish, by a core rule. Python sees floats
  and may do anything with them. Nothing flows back except commands.
- Column names, dtypes and extents come from the generated snapshot container instance
  (`design_data_container.md` §5.1), with the same schema descriptors as core state.
  Snapshots are **dense**: publish gathers only live rows, so every row is a live entity,
  there is no validity column and there is nothing to mask. Core storage has holes; a
  snapshot never does (`design_data_container.md` §5.1).
- **Identity is the `id` column, not the row position.**
  - An id is a `uint64`, its slot and its generation, and the only valid command target.
    No id value is issued twice (`design_data_container.md` §2.2). A row position means
    something *only* inside the snapshot it came from. An erase or create shifts later rows,
    so row `r` of the next snapshot may be a different entity.
  - `snap.find(type, id)` is the intended lookup. The `id` column is sorted for every object
    type, so `np.searchsorted` works as well (above).
  - A command naming an entity that has since died is rejected deterministically by the
    core. It is never undefined behaviour and never a silent hit on another entity: a later
    occupant of the slot has a different generation. This is normal reaction latency.
  - That is an application-time rejection (§7.1): submitted, recorded, rejected at
    consumption, and reported through that command's own outcome. Both of a command's
    answers go back to its submitter. Neither is an event.
- Snapshots are safe to share across threads without locks, because they are immutable.
- Snapshots are projections, not savegames (`design_engine_core.md` §5). They are lossy
  (fixed-point to float) and partial by declaration. State is restored by replay, or by a
  future save/load artifact, never by a snapshot round trip. Replay uses the full artifact
  of `design_engine_core.md` §2.3; "seed plus commands" is its shorthand. Persisting a
  snapshot is only a convenience for viz and analytics.

### 7.3 Events (outbound happenings)

Events report what happened in the core. They travel on the event ring to the owner thread,
which drains them once per frame.

```python
# pseudo-code
for ev in engine.drain_events():     # one batch per frame; never blocks
    ...
```

Rules:

- Events are tick-stamped, typed and drained in batches. `drain_events` never blocks, and it
  has exactly one owner (§6). The event ring it drains is bounded, and lossless up to its
  size. **That size is derived, not picked**: `≥ C × D` (§7.1).

  The ring is easy to size because neither of a command's two answers travels on it.
  Admission is `submit`'s return value, and the outcome is read from its handle (§7.1). What
  remains is created by the engine, so no source can flood the ring. Whether the events the
  engine creates stay within that size is open
  ([Q10](open_question.md#q10-can-the-event-ring-fill-within-one-tick)).
- **Backing up is not a failure state, and never ends the session.** Who is at fault decides
  the response (`design_engine_core.md` §5.2):

  | Queue | At fault | Response |
  |---|---|---|
  | a mod's own inbox | that mod is not draining | suspend that mod and report; the simulation continues (`design_modding.md` §4.2) |
  | the event ring | the application is not draining | pause the simulation and report. The gate blocks, as it does for `pause()` |
  | a peer falling behind | never decided locally; the server tier decides (`design_multiplayer.md` §4.1) | until the server tier answers: pause and report |

  Keeping the three apart stops the mildest fault from getting the harshest response. A
  pause is undone by whatever caused it: drain, and the gate opens.
- Fan-out to mods happens in Python, in the mod bus of §4.1, with one inbox per subscriber.
  Each inbox's size is declared in the manifest and capped by `HostPolicy.max_inbox_size`
  (§3, `design_modding.md` §4.2). On overflow, an inbox follows the event's delivery class
  (below). There is no separate per-mod overflow policy to declare (`design_modding.md`
  §4.2). A slow subscriber can never push back on the loop or the core. A mod that stays
  behind is suspended instead of being left to pile up events.
- **Delivery classes** (`design_engine_core.md` §5):
  - **Reliable/audit**: a system's create refused by a declared game-rule cap, a participant
    dropped at the gate, a mod suspended, an async failure, session or replay integrity. It
    does *not* include anything a command's submitter is still there to be told (§7.1).
    These events are never silently lost at any stage. Subscriber inboxes reserve space for
    this class, or keep a sticky loss counter, so a loss is always visible. Which of the
    two, and when an overflow suspends the mod instead, is open
    ([Q45](open_question.md#q45-protecting-reliable-events-in-mod-inboxes)).
  - **Coalescible state**: "sim behind", publish bandwidth, progress. Keeping only the
    latest is always safe.
  - **Best-effort telemetry and UI**: may be dropped.

  Dropping and coalescing apply to the last two classes only.
- **Event payload schema**: one canonical encoding, under the same rules as the command
  schema (§7.1). It uses explicit little-endian, fixed widths and full validation, and it
  has its own version. Readers reject a newer version instead of guessing. It has two uses,
  not three: the process-host IPC wire and the Python binding. It is not in the replay
  artifact, which carries commands only. A replayed tick re-derives its events by running
  the commands (`design_engine_core.md` §2.3, `design_modding.md` §4.3). So a new event type
  bumps the *event* schema version and leaves the replay format alone. The two version
  numbers move independently, which is why there are two.
- The full list of events and their cadence are still a core-side design item
  ([Q16](open_question.md#q16-which-events-exist)). This document fixes the binding shape,
  the three delivery classes and the scope of the payload schema.

## 8. Error handling

Every error that reaches Python has a Python type, and a caller can tell retryable errors
from terminal ones by type. The one exception is `ModLoadError`, which is retryable only
before the engine is touched. Errors on the sim thread are stored and raised again on the
owner thread.

Rules:

- Every C++ subsystem exception is bound (`nb::exception<x_error>(mod, "x_error",
  PyExc_RuntimeError)`, `design_patterns.md` §8) and surfaced under a Python-style name
  (`sim_estab.GpuError`, ...). No C++ exception ever crosses the boundary untranslated.
- **Async errors** (the sim thread of a windowed session): an error on the C++ sim thread
  is captured, and the sim thread stops safely. The exception is raised again at the next
  **rendezvous point** on the driving thread: any pump call, `stop_sim_async()`, or an
  explicit `engine.raise_if_failed()`. **The handoff is a three-state publication**
  (`design_engine_core.md` §1.1), never a Boolean that can become visible before its
  payload:

  ```text
  EMPTY = 0, WRITING = 1, READY = 2

  producer:
      if !state.compare_exchange_strong(EMPTY, WRITING, acq_rel): return
      fill captured-error slot with ordinary writes
      state.store(READY, release)              # publication LP

  rendezvous:
      if state.load(acquire) == READY: read captured-error slot
  ```

  The `EMPTY -> WRITING` claim makes the first error win, without publishing uninitialized
  storage. Later errors log only if useful and never touch the slot. `WRITING` means "not
  ready yet", so a rendezvous neither reads nor raises from it. It will see `READY` at a
  later rendezvous. Both sides are wait-free: the driving thread learns of a failure when it
  next asks, never by waiting.

  `close()` is not a rendezvous point, because it never raises (§2). An error still `READY`
  at `close()` is logged prominently instead, using the same acquire load. Errors still
  cannot be silently lost on the context-manager path, because `__exit__` runs
  `stop_sim_async()`, which raises, before `close()` (§2).
- Python-side validation errors (`ValueError`, `EngineClosedError`) raise before native code
  is touched.
- **Lifetime and shutdown raise sites have named types.** Callers must be able to tell
  retryable from terminal without matching on message text:

  | Raised when | Type | Retryable? |
  |---|---|---|
  | Any new call once closing has linearized, or any call on a closed engine or handle (§2, §6, §7.2) | `EngineClosedError` | no; terminal for that engine |
  | Any call once the engine is `failed`; `stop_sim_async()` on the final join timeout (§2, §4.3) | `EngineFailedError` | no; terminal, and the process is poisoned |
  | `view.take()` while an array view on the previous snapshot is alive (§7.2) | `ViewBusyError` | yes: drop the array views, or copy |
  | Any lifecycle or pump call off the owner thread (§2, §6) | `EngineThreadError` | yes: make the call from the owner thread |
  | A second `Engine` construction in one process (§2) | `EngineExistsError` | no; the resources are per-process singletons |
  | `step()` while the sim thread runs and is not paused, or with another `step()` in flight; `resume()` with a `step()` in flight; `run_sim_async()` in a headless session; `step()` or `run_sim_async()` once the session has stopped (§2, §4.3); `checksum()` while the sim thread is not quiescent (§6); any tick, command or snapshot call while `configuring`, or a `configuring` call (including `register_view`) while `running` (§2) | `EngineStateError` | yes: pause first, start the session first, or use `step()` in a headless session. A stopped session stays stopped |
  | `snap.column(...)` for a column outside the caller's capabilities (`design_modding.md` §4.1) | `ColumnNotGrantedError` | no; capabilities are fixed at load |
  | `load_mods` failing: a mod fails discovery, verification, handshake or column registration. Or `start_session` failing at the freeze's `on_session_start` (`design_modding.md` §6) | `ModLoadError` | only before the engine is touched. A policy-stage failure leaves the engine `configuring`, and `load_mods` may be retried. From `register_hosts` on, the engine is `load_failed`, and recovery is a new `Engine` |
  | A paced producer's `outcome(h)` for a tick it has not yet released (§7.1) | `CommandOrderError` | yes: release the tick first. It reports the deadlock rule at the call site instead of letting it become a hang. The host's `outcome(h)` never raises it: it returns `pending` |
  | The computed session identity differs from a loaded replay header (§4.2) | `ReplayIdentityError` | no; the artifact does not describe this build, schema or mod set |

  Neither of a command's two answers is in this table. Admission is `submit`'s return value,
  and the outcome is read from its handle. They are never exceptions and never events
  (§7.1), and that includes `queue_full`. `pending` is not in it either: it is an outcome
  that does not exist yet, returned like any other. `CommandOrderError` is the one nearby
  raise, and it is not an outcome. It reports that the *call itself* was made in an order the model
  forbids.

  `ViewBusyError` is one of the retryable types, which is why it is a distinct type rather
  than a kind of `RuntimeError`. A mod that catches it can retry on its next idle slice.
  Catching `EngineClosedError` or `EngineFailedError` only hides a dead engine.
- Fail loudly and early. A failed construction leaves no partial state (C++ constructor
  rollback). A failed `run_*_async` leaves the engine in the plain `running` state.

## 9. Checklist for new Python API surface

Go through this list for every new piece of Python API.

1. New C++ API? Bind it 1:1 in `bind.cpp`, following `design_patterns.md` §8. Put the
   Pythonic layer in pure Python above it.
2. Can the call run long? Add the GIL-release call guard (§5 rule 2).
3. New data flowing to Python? It is a snapshot, an event or a query, never per-entity call
   traffic (§5 rule 1). Make it zero-copy where it recurs every frame.
4. New data flowing in? It is a command (recorded and validated) or a construction-time
   option. Nothing else changes the world. A command names its tick and gets two answers,
   admission and outcome (§7.1). A new inbound surface that returns one answer, or none, is
   a new model, not a new call.
5. State the thread contract explicitly (§6): owner-thread-only, owner-thread-and-quiescent,
   externally synchronized, thread-safe, or immutable and shared. A new raise site gets a
   named type in the §8 table in the same change. If the new surface adds a cross-thread
   mechanism instead of using an existing one, it gets a row in the mechanism register
   (`design_engine_core.md` §1.1) in the same change. The row needs all five items: atomic
   objects and widths, the memory order of each operation, the linearization point, the
   progress class, and the failure and deadline behaviour. Naming a data structure is not
   specifying one.
6. Errors are translated, and their messages are specific (§8).
7. Tests run on both `cp314` and `cp314t`, and the GIL-status guard test stays green.

## References

- Prior-art survey: [openage
  pyinterface](https://github.com/SFTtech/openage/blob/master/doc/code/pyinterface.md) ·
  [openage
  architecture](https://github.com/SFTtech/openage/blob/master/doc/code/architecture.md) ·
  [openage main.cpp (`run_game` →
  `engine.loop()`)](https://github.com/SFTtech/openage/blob/master/libopenage/main.cpp) ·
  [Panda3D main
  loop](https://docs.panda3d.org/1.11/cpp/programming/tasks-and-events/main-loop) · [Panda3D
  tasks](https://docs.panda3d.org/1.10/cpp/programming/tasks-and-events/tasks)
- SDL3 threading constraints:
  [SDL_WaitAndAcquireGPUSwapchainTexture](https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture)
  · [SDL3 GPU category (command buffers across
  threads)](https://wiki.libsdl.org/SDL3/CategoryGPU) · [render-thread swapchain issue,
  closed as not planned](https://github.com/libsdl-org/SDL/issues/12959)
- Loop structure: [Gaffer On Games — Fix Your
  Timestep!](https://gafferongames.com/post/fix_your_timestep/)
