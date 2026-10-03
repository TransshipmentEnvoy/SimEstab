# Modding Design: Tiers, Mod Hosts, Determinism, Audit

This document designs the SimEstab mod system. Python is both the host application and the
mod VM. (In openage, by contrast, Python is a guest scripting VM under a C++ loop.) So mod
support is mostly a problem of loading, isolation and contracts, not of embedding.

Status: designed, not implemented. Nothing here is built. v1 ships the thread host first
(§4.3). Process hosts come later, and their snapshots are deferred together with the
`SHARED` engine view mode (`design_engine_core.md` Appendix A).

Terms are defined in [glossary.md](glossary.md). This design builds on
`design_python_api.md` (main loop, boundary artifacts, thread contract) and
`design_engine_core.md` (commands and snapshots, determinism, and the mind/body split of
§6).

Scope of v1:

- **A process host has no snapshot.** Its snapshot design (§4.3) is an engine view placed in shared
  memory, and that needs the multi-reader `SHARED` mode, which v1 does not build. A
  `PRIVATE` engine view cannot face a sandbox. A process host keeps its commands, events and
  lifecycle. How `SHARED` returns is open
  ([Q60](open_question.md#q60-reviving-the-shared-engine-view)).
- Thread hosts are trusted, so `ctx.snapshot()` and an opt-in `PRIVATE` engine view both work as
  described here.

How to read this doc:

- Long sections start with a **TL;DR**.
- **Pattern** is the generic contract; **Example** is a concrete sketch.
- All code in this doc is **pseudo-code**: shapes and contracts, not final names.
- A deviation from these rules needs a documented reason.

---

## 1. Tier overview

Mods come in three tiers. Each tier has more power than the one before and costs more trust.

**TL;DR**

- Three tiers, each with more power and a higher trust cost than the one before.
- Tiers 1 and 2 live outside the determinism boundary and change the world only through
  recorded commands. Tier 3 lives inside it and is trusted code.

| Tier | Form | Runs | Changes the world through | Trust and isolation |
|---|---|---|---|---|
| 1 **Content** | declarative data (units, stats, recipes) | loaded into the core before the freeze (§6) | nothing: it *is* the initial state | hashed into session identity |
| 2 **Logic** | Python package | its own **thread** or its own **process** | commands only | limited by capabilities; can be isolated |
| 3 **Core** | native `.so` / `.dll` | inside the deterministic tick | registering systems directly | fully trusted; audited and certified |

Rules:

- Prefer the lowest tier that can express the mod. New unit types, stats and recipes are
  content, not code: data that deterministic C++ systems interpret. This is the lesson of
  openage and nyan.
- **There is no "Python inside the tick" tier.** It would put the GIL and Python's
  non-determinism inside the determinism boundary, and cross that boundary once per mod per
  tick.

## 2. The peripheral rule: why logic mods cannot desync

A logic mod is built like a player or an AI peripheral (`design_engine_core.md` §6). This
section lists what follows from that.

**TL;DR**: a logic mod is a non-deterministic brain whose recorded signals are deterministic
inputs.

- **Inbound**: only tick-stamped commands. They are validated and **recorded into the input
  stream**, so replays capture mod behaviour for free.
- **Outbound**: immutable snapshots and the event stream. Reads can be stale; that models
  reaction latency and is not a bug.
- **Consequences**: mod Python may use floats, the GC may pause, and hash randomization does
  not matter. None of it can make a replay diverge from the recorded command stream
  (transition determinism, `design_engine_core.md` §2). Mod timing does shape which commands
  a live session queues and how they are stamped. That changes the stream being recorded,
  not the determinism of replaying it. Session reproducibility is not promised.
- **How far a crash reaches depends on the mod host (§4.3):**
  - **Python exception, any mod host**: the mod host catches it and quarantines the mod. The
    engine is unaffected. Only that mod's future commands are lost.
  - **Native crash, thread host** (a bad C extension): kills the engine process, because a
    thread host has no crash boundary. This is why mods heavy on native code should declare
    the process host.
  - **Native crash, process host**: fully contained. The engine reaps the dead process once
    its death is confirmed (§4.3). Once process-host engine views exist, it also unlinks the
    process's engine view segment then.

  In every case sim *state* is untouched. Determinism is never lost; at worst the process
  is.
- **A mod names the tick its command acts on**, and the engine runs the command at that tick
  or refuses it (`design_engine_core.md` §5.1). So where a mod computed, and for how long,
  never affects the core beyond what is recorded. A slow mod names a later tick, which is a
  visible choice; no tick is chosen for it. A mod's stamp margin is 0: it acts inside the
  tick it is about to release. So naming any other tick is a bug that is rejected, not a
  surprise that is queued.

## 3. Manifest and capabilities

Every mod ships a static manifest that says who it is and what it may do. The engine binds
each mod to a source id and enforces its capabilities per source id. This section defines
both.

**TL;DR**

- Every mod ships a static manifest: identity, tier, mod host, capabilities.
- The engine enforces capabilities per source id, at the command ring.

### Pattern

```toml
# pseudo-code — mod.toml
[mod]
id       = "example.traffic"        # unique, stable principal NAME (not the source id)
version  = "1.2.0"
tier     = "logic"                  # content | logic | core
host     = "thread"                 # logic only: thread | process
requires = ["example.roads>=2"]     # dependency ordering

[capabilities]
commands = ["set_route", "spawn_vehicle"]   # command types it may submit
events   = ["vehicle.*", "session.*"]       # event topics it may subscribe to
columns  = ["vehicle.position", "vehicle.id"]   # column read grants (§4.3;
                                                #   design_data_container.md §7.2)

[limits]
commands_per_tick = 4096                    # requested per-tick endpoint capacity;
                                            #   engine-capped (design_python_api.md §7.1).
                                            #   The number here and the 256 cap in
                                            #   doc/design_limits.md §2 disagree, and both
                                            #   stay as they are ON PURPOSE: this field's
                                            #   meaning is still shifting, so reconciling
                                            #   the numbers first would fix the wrong thing
                                            #   (doc/design_limits.md §7)
private_view      = false                   # true = a dedicated PRIVATE engine view over the
                                            #   granted columns; costs one projection
                                            #   copy per publish (§4.1). Default: copy
                                            #   from the engine's default engine view.
inbox_size        = 256                     # per-subscriber inbox capacity (§4.2);
                                            #   overflow behaviour is not declared —
                                            #   it follows the event's delivery class
```

The manifest holds the per-mod settings the rest of this document enforces: the endpoint's
per-tick command capacity, the inbox size and the column capabilities. There is no inbox
overflow setting; overflow follows the event's delivery class (§4.2). Two settings have no
field yet: a mod's gate deadline and expiry policy
([Q9](open_question.md#q9-how-does-a-stopped-participant-leave-the-gate)), and an endpoint
per producer thread (§4.1,
[Q66](open_question.md#q66-how-does-a-mod-declare-an-endpoint-per-producer-thread)).

The example requests `commands_per_tick = 4096`, above the endpoint capacity cap of 256
(`design_limits.md` §2). Both numbers stay as they are until the field's meaning is settled
([Q44](open_question.md#q44-what-commands_per_tick-in-a-manifest-means)). When
`private_view` is false, a thread-host mod reads the world through `ctx.snapshot()`, a copy
out of the default engine view (§4.1). A process host has no snapshot in v1.

Rules:

- **Principal name and source id.** The manifest `id` is the stable, human-readable name.
  The source id is a `u32` the engine **assigns at the freeze**, from the deterministic load
  order below (`design_engine_core.md` §2.4 step 3). 0 is reserved for the host. For each
  Tier 2 principal outside a replay, the freeze also allocates a stable endpoint and binds
  the id to it.
  - Allocating an endpoint and opening it are separate moments. A replay assigns every id
    and creates no Tier 2 endpoint at all. A live mod host opens its pre-bound endpoint only
    at spawn. So a failed spawn cannot renumber the other mods.
  - Commands carry the number, so the replay header records the **map between names and
    source ids**. Without it, a recorded stream could not be traced back to a named mod.
  - The core orders each tick's commands by (source id, sequence) (`design_engine_core.md`
    §5). The drain assigns both; the mod never supplies them (`design_python_api.md` §7.1).

  The same id also keys:
  - capability checks at submission: a command type outside the manifest is rejected and
    logged, never applied;
  - per-mod attribution in the replay;
  - rate limiting and revocation. Each endpoint declares one number, its **per-tick
    capacity**. The manifest declares it and the engine caps it. The ring depth derives from
    it: a mod's margin is 0, so its ring holds `capacity` entries (`design_limits.md` §2).
    There is no separate drain quota to exceed, because a tick runs everything stamped for
    it. A mod that submits more than it declared for one tick gets `queue_full` **from the
    submitting call**, and nowhere else (`design_python_api.md` §7.1). A mod within its
    capacity never finds its ring full (`design_engine_core.md` §5.1). Neither of a
    command's two answers is an event, so a mod cannot flood the event ring by submitting hard. Rings
    are per endpoint, so one mod's burst cannot use up another's capacity at all.
- **The engine binds identity; a mod cannot declare it.** The source id is bound to the
  submission endpoint the engine allocates at the freeze and opens for each mod host at
  spawn: `ctx.submit*` in-process, or the per-process IPC channel for a process host. It is
  never read from command bytes the mod provides, and never borrowed. A mod submits its
  commands on that endpoint only. Relaying them through the host's endpoint would bind
  capacity, capability checks, attribution and revocation to the wrong source
  (`design_python_api.md` §4.1, §7.1). On a thread host this binding is a convention, like
  every in-process boundary (§4.3). On a process host it is enforced, because the OS channel
  identifies the sender.
- **Capabilities are declared, inspectable and cannot grow at run time.** They are a ceiling
  the mod declares. Policy may lower it and nothing may raise it (§3.1). Showing
  capabilities to a user before a mod is installed is the job of whatever installs mods. The
  engine's loader reads the manifest and never asks a question.
- **Load order** is a deterministic topological sort of `requires`, with ties broken by mod
  id. It never depends on the order in which the filesystem lists files.
- The manifest of every loaded mod (id, version, content hashes) is part of the replay
  header, so a replay knows exactly which mods produced it. The freeze writes it, after
  every mod has loaded (`design_engine_core.md` §2.4). It is never written piece by piece as
  mods arrive, so it cannot describe a load that did not finish. In multiplayer every peer
  needs the same mod set; comparing it between peers is open
  ([Q54](open_question.md#q54-enforcing-the-same-mod-set-on-every-peer)).

### 3.1 `ModPolicy`: what the user allows

The manifest is what a mod asks for. `ModPolicy` is what the user allows. It is plain data
passed to `load_mods`, never a dialogue.

`ModPolicy` is an argument to `load_mods` (§6), never a field of `EngineConfig`. It drives
filesystem discovery and per-mod verification. That is Python policy code that can fail one
mod at a time, and the `Engine` constructor must not run such code (`design_python_api.md`
§2). Nothing in this type or in `load_mods` is interactive.

```python
# pseudo-code — sim_estab.mod
@dataclass(frozen=True, slots=True)
class ModPolicy:
    paths: tuple[Path, ...] = ()             # where to discover mod.toml; the engine
                                             #   never scans directories itself (§5.1)
    require_signature: bool = True           # unsigned/unlisted -> refuse (§5.4)
    allowlist: tuple[str, ...] = ()          # mod ids or content hashes (§5.4)
    allow_uncertified: bool = False          # load mods that fail certification (§5.3)
    deny: tuple[str, ...] = ()               # capability strings no mod may be granted,
                                             #   whatever its manifest declares
    host_override: Mapping[str, str] = {}    # mod id -> "thread" | "process" (§4.3)
    replay_observers: bool = False           # spawn Tier-2 hosts admission-closed during
                                             #   playback (design_python_api.md §4.2)
```

Rules:

- **Capabilities are resolved, never negotiated.** A mod's effective capabilities are its
  manifest's `[capabilities]` block minus anything in `deny`. `load_mods` never asks a
  question or waits for an answer. Whether a user is *shown* those capabilities, and where
  they approve installing a mod, belongs to whatever installs mods: a launcher, a package
  manager, a text editor. The engine's loader has no part in it. A confirmation step here
  would put a blocking prompt inside a function that CI and the replay harness both call. It
  would then need a "don't ask" mode at once: a setting standing in for a decision someone
  else owns.
- `deny` is the policy's only control over an individual capability, and it subtracts.
  Policy cannot give a mod anything its manifest did not declare. Capabilities are a
  *ceiling* the mod declares, and policy can only lower it.
- **Policy selects; the freeze records what was selected.** No `ModPolicy` field enters the
  replay header. What enters is the *result*: the manifest list, versions and hashes of what
  actually loaded (`design_engine_core.md` §2.3, §2.4). So two users with different policies
  who end up with the same mod set produce the same identity. No policy field can become an
  undeclared determinism input.
- **What the engine enforces lives elsewhere.** These limits are in
  `EngineConfig.host_policy: HostPolicy` (`design_python_api.md` §3): the shutdown budget
  (the wait for running submits, inbox drain, join, confirm death), the IPC deadline, the
  quarantine retry limit and the inbox size limit. The engine needs them for the whole
  session and inside `close()`, whether or not `load_mods` was ever called. So they are
  frozen at construction. The dividing line is the one §5.1 draws: Python owns policy, the
  engine owns mechanism.
- **`host_override` may only move a mod to a more isolated mod host** than its manifest asks
  for (`thread` → `process`). An override the other way is a **`ValueError` at `load_mods`,
  not a silent downgrade**. A mod that declared `process` did so because its author expects
  a crash or trust boundary. Removing that quietly is the one policy change that can turn a
  contained failure into a dead engine (§2).

## 4. Tier 2: logic mods

A logic mod is a Python package that runs in a mod host, outside the core. It reads events,
and snapshots on a thread host (a process host has none in v1). It changes the world only by
submitting commands. This section covers the API a mod uses (§4.1), the mod host loop (§4.2)
and the two kinds of mod host (§4.3).

**TL;DR**

- Mods never run on the main loop. They run in mod hosts: a **thread host** (the default,
  cheap) or a **process host** (opt-in, for isolation).
- A mod is written against a `ModContext` capability object only. That is the supported
  surface. On a thread host it is a convention; only a process host enforces it physically
  (§4.3).
- One payload encoding serves in-process calls, IPC and replay. But the **replay artifact
  carries commands only**, so events share the encoding rules, not the artifact (§4.3).

### 4.1 Pattern: the mod protocol and `ModContext`

A mod implements a few callbacks, and its mod host calls them. The mod reaches the engine
only through `ModContext`, which lists every call it may make.

```python
# pseudo-code — what a mod author writes
class TrafficMod:
    def on_load(self, ctx): ...          # session already running (§6): read config,
                                         #   content, or an initial snapshot
    def on_event(self, ctx, ev): ...     # subscribed events, batched
    def on_idle(self, ctx): ...          # optional periodic slot (host-paced)
        def on_outcomes(self, ctx, outcomes): ...  # outcomes of this mod's commands,
                                               #   for ticks already released (§4.2)
    def on_unload(self, ctx): ...

    def _pick_target(self, ctx):                 # own frame: views die on return
        with ctx.snapshot() as snap:             # a copy out of the engine's default engine view
            ids = snap.column("vehicle.id")
            return int(ids[0])

    # ... then, the only write path:
    #   ctx.submit(command.set_route(vid, path))
```

`on_outcomes` is called by the mod host loop after it releases a tick, with the outcomes of
the commands the mod submitted for that tick (§4.2).

`ModContext` exposes exactly the following:

- `submit(cmd, tick)` / `submit_batch(cmds, tick)`: **the only write path for this mod's
  commands.** They submit on the mod's own endpoint, check capabilities, and name the tick
  the commands act on. They return **admission** per command, which the mod must inspect:
  `admitted{handle}`, `queue_full`, `too_late`, `out_of_order`, `over_margin`, `invalid`,
  `revoked` or `host_error`.
  - There is no buffer-and-flush step: `submit_batch` *is* the batching primitive.
  - A mod's stamp margin is 0, so the only tick it may name is the one it is about to
    release. Any other tick is `over_margin`.
  - At margin 0 the ring never fills while the mod stays within its capacity, so `submit`
    does not wait for space (`design_engine_core.md` §5.1). `too_late` means the mod named
    a tick that has run, or one it has already released.
  - A pause wakes no waiting call. An outcome wait stays parked and continues when the
    session does; only revocation wakes it early, with `revoked`.
  - `queue_full` means one thing only: "more than the capacity this mod declared for one
    tick". That is the mod's own contract, not backpressure.
  - The endpoint has a **single producer**, and a source has one endpoint in v1. A mod that
    submits from more than one of its own threads serializes them. Whether a mod may later
    declare an endpoint per thread is open
    ([Q66](open_question.md#q66-how-does-a-mod-declare-an-endpoint-per-producer-thread)),
    and so is how the drain would order two endpoints of one source
    ([Q62](open_question.md#q62-in-what-order-are-two-endpoints-of-one-source-drained)).
- `next_tick()`: the tick this mod is about to release. The mod host loop names it in every
  submit, then releases it (§4.2).
- `declare_ready(tick)`: **releases the tick.** Submitting commands makes a mod a
  participant (`design_engine_core.md` §3.3). So the engine does not advance past a tick
  until the mod declares it has finished submitting for it. Every mod is paced; there is no
  unpaced mod and no opt-out, because a mod the engine does not wait for is a mod whose
  commands can be dropped. Whether a mod must really be paced every tick is open
  ([Q2](open_question.md#q2-must-every-mod-be-paced-every-tick)).
- `outcome(handle)` / `outcomes(handles)`: **what a command did** when it ran: applied, or a
  named rejection at application time. This is a second wait, and a different one. Admission
  asks "is it queued?"; the outcome asks "what happened?", and that answer does not exist
  until the tick runs. **Only ticks already released may be asked about.** Asking about a
  tick the mod is itself holding up raises `CommandOrderError` at once, instead of hanging
  the session (`design_engine_core.md` §5.1). So the natural order is submit, then
  `declare_ready`, then outcome. The loop in §4.2 follows it.
- `snapshot()`: a copy out of the engine's default engine view (`design_python_api.md` §7.2). A mod
  may take any number of them, from any thread
  ([Q3](open_question.md#q3-can-enginesnapshot-be-called-from-any-thread)), and a snapshot
  outlives everything. There is no lifetime rule and no refusal, though the call still
  raises the normal engine lifecycle errors. It is the ordinary way a thread-host mod reads
  the world, and its cost is the copy. A process host has no snapshot in v1 (§4.3).
  - **A snapshot is already a copy**, so carrying state from one tick to the next needs
    nothing special. There is no pin to hold and no quota to split. What one mod reads never
    affects what another can read.
  - **Zero-copy is opt-in and declared.** A mod that reads a large slice every tick may
    declare a `PRIVATE` engine view in its manifest. It is registered at the freeze and costs one
    projection copy per publish. The manifest shows that cost, and the mod that asked for it
    pays it. The mod reads it through `view()`, below.
  - The default is `snapshot()`, and most mods should stay on it.
  - **A row position means something only inside its own snapshot.** Identity is the
    projected `id` column, and a command names an id, never a row position. Row `r` of one
    snapshot says nothing about row `r` of the next (`design_data_container.md` §2.2, §5.1).
    Resolve an id with `snap.find(type, id)`. The `id` column is sorted for every object
    type, so one lookup rule holds everywhere (`design_python_api.md` §7.2).
  - **The snapshot catalog holds only the columns in the mod's capabilities.** Asking
    `snap.column(...)` for any other column raises `ColumnNotGrantedError`. On a thread host
    this is not a security boundary. It keeps the *observable surface* the same as on a
    process host, whose engine view (once process-host engine views exist) contains only those columns
    (§4.3). So moving a mod to another mod host by policy cannot silently change what it can
    read.
- `view()`: the mod's own `PRIVATE` engine view, if its manifest sets `private_view = true`. It
  follows the one rule of `design_python_api.md` §7.2: `take()` is refused while array views
  on the previous snapshot are still alive.
- `log(...)`: logs on the channel `sim_estab.mod.<id>`, following the hierarchical channel
  convention of `design_patterns.md` §7.
- `stopping`: an **advisory** flag for mod host teardown. Shutdown sets it before or
  together with endpoint revocation, and wakes the mod host loop. But polling it is not an
  endpoint lease. `False` followed by `ctx.submit*() -> revoked` is a legal race, and the
  submit result is what counts. A call that entered its endpoint lease before revocation may
  instead finish with an ordinary result while `stopping` is already true. It is listed here
  because the mod host loop reads it.
- **There is no `subscribe()` call.** The manifest's `events` capability is the only
  subscription. The inbox is created at spawn, already subscribed. So there is no window
  between "this mod host exists" and "this mod host is subscribed" for an event to fall
  into. A runtime call could only narrow the subscription within capabilities the user
  already accepted. A narrowing that arrives after the inbox started filling would have to
  say what happens to queued events of a topic no longer wanted. That question has no
  acceptable answer for the reliable class (§4.2). A mod that wants fewer topics declares
  fewer topics. The same reasoning is why a mod declares no inbox overflow policy (§4.2):
  the delivery class already decides it.
- Nothing else: no engine lifecycle calls, no pump calls, no access to other mods.

On a thread host, the API shape is the supported surface and guards against accidental
misuse. It is not security enforcement, because in-process Python can reflect past it
(§4.3). The peripheral rule is enforced where enforcement is physical. Writes are checked at
the command ring, on every mod host. Reads are limited by the engine view's projection spec. Once
process-host engine views exist, the columns in a process host's capabilities are the only bytes it
can see (§4.3).

### 4.2 Pattern: the mod host loop

Each Tier 2 mod runs in its own mod host loop. The loop feeds the mod its events, submits
and releases one tick at a time, and then reads outcomes. This section also defines what
happens when a mod falls behind (suspension) or fails (quarantine).

```python
# pseudo-code — one host per mod (thread variant)
def host_loop(mod, ctx, inbox):
    mod.on_load(ctx)                  # session already running (§6)
    pending = []
    while not ctx.stopping:
        t = ctx.next_tick()           # the tick this mod is about to release
        batch = inbox.get_batch(timeout=IDLE_SLICE)   # blocks; main loop never does
        for ev in batch:
            mod.on_event(ctx, ev)     # the mod submits via ctx.submit*(cmd, t) itself,
        mod.on_idle(ctx)              #   on this thread, collecting handles in `pending`
        ctx.declare_ready(t)          # RELEASES t. Every mod is paced (§4.1)
        mod.on_outcomes(ctx, ctx.outcomes(pending))   # only ticks already released
        pending.clear()

    for ev in inbox.drain():          # shutdown step 2 (§6): what was already delivered
        mod.on_event(ctx, ev)         #   is still delivered — the inbox is not discarded
    mod.on_unload(ctx)                # endpoint already revoked (§6): may not submit
```

`IDLE_SLICE`, the loop's timeout while it waits for events, has no value yet
([Q21](open_question.md#q21-what-are-the-catch-up-clamp-and-idle_slice)).

Rules:

- **The main loop carries no command traffic.** It only fans events out into per-mod inboxes
  with `bus.publish()`, which never blocks (see the reference loop in `design_python_api.md`
  §4.1). A mod submits from its own mod host thread. So no mod's batch can make the main
  loop pay O(commands) before `render()`.
- **The loop's shape is the deadlock rule** (`design_engine_core.md` §5.1), not a matter of
  style. Submit for `t`, release `t`, *then* read outcomes for `t`. A mod that read outcomes
  before `declare_ready` would be waiting on the tick it is itself holding up. The engine
  refuses that call outright instead of letting the session hang.
- **Backpressure.** A slow mod lags *its own* inbox, never the main loop, the core or
  another mod. A mod that stays behind risks **suspension** (below), not a growing queue.
  - Overflow follows the event's delivery class, not a per-mod setting: coalescible state is
    merged, and best-effort events are dropped. A mod declares its inbox *size* and nothing
    else. A second setting could only contradict the class it applies to.
  - **An inbox that overflows suspends its mod.** The mod is at fault, since it is the one
    not draining. So the response is limited to that mod, and the simulation continues.
    `design_engine_core.md` §5.2 lists this case beside the other two overflow cases; each
    response is scoped to whoever is at fault.

    **Suspend** has one meaning:

    | Aspect | Rule |
    |---|---|
    | Reversible | yes. Suspension is not a partial unload |
    | The mod | stops being fed: no events, no snapshots |
    | Its endpoint | **kept**, with whatever it already holds. Nothing is discarded and nothing is renumbered |
    | Pacing | it **leaves the gate's conjunction** while suspended (`design_engine_core.md` §3.3, `on_expiry = SUSPEND`) |
    | Resuming | once it drains. Feeding restarts, and it re-enters pacing at the current tick |
    | Reported | yes, as a reliable-class event naming the mod |

    Leaving pacing is required. Every mod is a participant, so a suspended mod that stayed
    in the conjunction would hold the gate forever. One mod's overflow would then stop the
    simulation. The only two answers are "the mod leaves pacing" and "the simulation
    pauses". Because every mod is paced, this is the only case in which a mod leaves pacing.
    The memory order of the `SUSPEND` store is unspecified
    ([Q9](open_question.md#q9-how-does-a-stopped-participant-leave-the-gate)).

    **Unloading a suspended mod is out of scope**
    ([Q47](open_question.md#q47-unloading-a-suspended-mod)). It is a separate question.
    Unloading means reclaiming an endpoint whose ring may still hold accepted commands, and
    retiring a source id that is in the replay header. Suspension avoids both by removing
    nothing.
  - The declared inbox size is **capped by the engine**, like the endpoint's command
    capacity: `min(inbox_size, HostPolicy.max_inbox_size)`, with the reliable-class
    reservation as its floor (`design_python_api.md` §3, §7.3). Without a cap, total inbox
    memory would grow with the number of mods the user installed, and would be unknown at
    construction. A per-mod engine view allowance would have the same scaling defect
    (`design_engine_core.md` §3.1).
  - Reliable-class events are never silently lost. The inbox reserves space for them or
    keeps a sticky loss counter (`design_python_api.md` §7.3). This section uses both: the
    reservation floors the inbox size above, and quarantine reports loss through the counter
    below. How the two fit together, and when an overflow suspends the mod instead of
    dropping or merging by delivery class, is open
    ([Q45](open_question.md#q45-protecting-reliable-events-in-mod-inboxes)).
  - This is queue isolation, **not CPU isolation**. On GIL builds, a busy thread-host mod
    still takes time from the main loop (§4.3). Mods that are heavy on CPU, or untrusted,
    belong in a process host, where OS limits are the hard enforcement.
- **A mod exception is caught by its mod host.** It is logged with the mod id, and the mod
  is **quarantined**: its mod host stops and its commands stop flowing. The engine keeps
  running. How a quarantined mod leaves the gate is open
  ([Q9](open_question.md#q9-how-does-a-stopped-participant-leave-the-gate)).
  `HostPolicy.mod_retry_limit` sets how many times the mod may be re-spawned
  (`design_python_api.md` §3); `0` leaves it disabled. The limit lives on the engine side,
  because a retry re-binds an engine-owned source id, and that id is in the replay header.
  - **Retry delivers each command at most once, because admission is synchronous.** Commands
    submitted before the exception stay submitted and recorded. The mod already holds an
    admission result and a handle for every command it submitted. Nothing is re-submitted on
    its behalf. *Outcomes* for those commands die with the mod host; that follows from the
    mod host dying and is not a gap in the channel.
  - Retry re-spawns the mod host against the **same stable endpoint and ring**. The endpoint
    reopens only after the old producer has stopped and no submit still holds its endpoint
    lease (`design_engine_core.md` §5.1). Commands accepted before the exception stay in
    FIFO order and keep draining. Reopening never copies or re-queues them. This gives
    at-most-once delivery without replacing a ring that may still hold accepted commands.
  - The inbound path is not lossless across a retry. The quarantined mod host's inbox dies
    with it. So events fanned out while the mod was down, reliable-class included, never
    reach the re-spawned instance. Quarantine is therefore a **loss event for that mod's
    inbox**. It is reported through the same sticky loss counter as any reliable-class drop
    (`design_python_api.md` §7.3), and `on_event` after a retry may have missed history. A
    mod that cannot tolerate this must rebuild its state from a snapshot in `on_load`. That
    is one reason `on_load` runs after the freeze (§6) and can read one. In v1 only a
    thread-host mod can do this, because a process host has no snapshot (§4.3).
- **Per-mod accounting from the start**: events consumed, commands submitted, mod host CPU
  time. A misbehaving mod must be *attributable*. Attribution feeds policy. A thread host's
  only lever is a CPU budget per mod host, with health telemetry (budget-exceeded events).
  Hard CPU enforcement exists only in the process host, through OS limits or cgroups.

### 4.3 Thread host and process host

A Tier 2 mod runs in one of two kinds of mod host. The thread host is the default and costs
almost nothing. The process host adds isolation, paid for with IPC. v1 ships the thread host
first.

| Aspect | Thread host (default) | Process host (opt-in, by manifest or policy) |
|---|---|---|
| Snapshot access | an engine view in engine memory: the default engine view through `ctx.snapshot()`, or a dedicated `PRIVATE` engine view if the manifest asks for one | **none in v1.** A `PRIVATE` engine view cannot face a sandbox, so process-host snapshots wait for the `SHARED` mode (`design_engine_core.md` Appendix A). Their design is below |
| Commands | submitted in-process, straight into the endpoint's ring | IPC, using the canonical command payload schema (identical to replay files) inside a transport-specific envelope |
| Events | delivered in-process, into the inbox | IPC, using the canonical **event** payload schema: the same encoding rules, but **not** in replay files (below) |
| `ctx.submit_batch` | a direct call; the call returns admission | one frame out and one reply frame back from the supervisor; the call returns admission |
| `ctx.outcome` | a direct call; blocks until the named tick has run | **its own operation**, not a hot-path round trip (see the rule below) |
| Crash isolation | none: a bad C extension kills the engine | full. Once process-host engine views exist, the engine view segment is unlinked when process death is confirmed |
| Column read capabilities (`design_data_container.md` §7.2) | **enforced at the `ModContext` API**: other columns are absent from the catalog | no snapshot in v1. Once process-host engine views exist: enforced at the API **and physically**, because the engine view's projection spec *is* the capability set, so the segment contains nothing else |
| CPU isolation, GIL builds | **none**: a busy mod takes main-loop time through GIL contention | full |
| CPU isolation, free-threaded builds | good (true parallelism) | full |
| Security boundary | none: Python can reflect into anything | real: a subprocess with rlimits and reduced privileges |
| Cost | about zero | IPC mod host machinery, plus shared memory once process-host engine views exist |

Rules:

- **A mod written against `ModContext` only runs on either mod host.** The manifest, plus
  the user's `ModPolicy`, picks the mod host. No mod code changes between mod hosts. For
  everything a process host has, no observable behaviour differs: same calls, same
  capabilities, same results, same error types. The exception in v1 is snapshots, which a
  process host does not have. Otherwise only cost differs (the table above). In particular,
  a single `ctx.submit` costs one IPC round trip on a process host, so `submit_batch` is the
  idiom there. A mod that is correct on one mod host is correct on the other, as long as it
  does not read snapshots. It may be slower.
- **Command frames are bounded, and the bound is derived.** A child sends length-prefixed
  frames. Without a cap, an untrusted mod could drive engine-side allocation by declaring a
  huge length, which is what a process host exists to contain. The cap is not a tuned
  number. The largest *useful* frame is bounded by the endpoint's ring depth, since nothing
  beyond that can be admitted for one tick (`design_python_api.md` §7.1). A frame that
  declares more is rejected before allocation, and the endpoint is marked broken. In-process
  there is no such limit: there is no frame, and an oversized batch costs only the caller's
  own thread.
- **Each process host has one engine-side supervisor thread.** It reads the child's IPC
  frames, submits on that mod's endpoint, and writes the reply frames. It is the endpoint's
  single producer, as the SPSC contract requires (`design_engine_core.md` §5.1). The round
  trip is child ↔ supervisor, under a transport deadline, `HostPolicy.ipc_deadline`
  (`design_python_api.md` §3). When it expires, the call returns `host_error` and the
  endpoint is marked broken, which leads to quarantine. **The supervisor does no snapshot
  work.** Once process-host engine views exist, the sim thread publishes straight into the child's
  segment (below), so there is no broker and no second copy.
- **A waiting call needs its own frame shape, and the hot-path deadline must not apply to
  it.** A process mod has two waits: the admission wait and the outcome
  (`design_engine_core.md` §5.1). Both waits can rightly
  span a tick. Measured against a 33.3 ms tick, a 10 ms hot-path deadline would expire on
  every one of them. So a single request-and-reply frame shape cannot carry them:

  | Aspect | Hot-path submit | Waiting call (admission wait or outcome) |
  |---|---|---|
  | Frame | request → reply | request → *pending* → reply; the child can cancel it |
  | Bounded by | `ipc_deadline` | nothing of that kind: the engine answers when the tick runs |
  | Expiry means | the transport is broken: `host_error`, and the endpoint is broken | **"still waiting"**: a keepalive, never "kill the mod" |
  | Cancellable | no | yes, by the child, at any time |

  With the two shapes separate, `ipc_deadline` bounds only a round trip that should never
  span a tick. Its ceiling is the input delay, and its value is open (`design_limits.md`
  §1.1, §7). The 10 ms figure above is an example, not a decided value
  ([Q43](open_question.md#q43-ipc_deadline-value-and-constraints)). A deadline shorter than
  a tick can only ever detect "the other side is mid-tick", which is not a fault.
- **Ship the thread host first.** Add the process host when the first heavy or untrusted mod
  needs it.
- On GIL builds, policy should default heavy mods to the process host. On free-threaded
  builds the thread host is fast enough, though isolation may still call for a process host.
- **One encoding, two schemas.** The difference matters (`design_python_api.md` §7.1, §7.3).
  The **command** and **event** payload schemas use the same encoding rules: canonical
  little-endian, fixed widths, fully validated (`design_engine_core.md` §2.3). Each is
  versioned on its own, and a reader rejects a newer version instead of guessing. They
  differ in where each is used:

  | Schema | Used by |
  |---|---|
  | command payload | the replay artifact, the IPC wire, the Python builders |
  | event payload | the IPC wire and the Python binding; **never the replay artifact** |

  The replay artifact is a **command stream** (`design_engine_core.md` §2.3). A replayed
  tick *re-derives* its events by running the recorded commands. Recording events too would
  duplicate a deterministic output, and give a loaded replay two sources for the same fact.

  The process host must not invent a second payload serialization for either schema. Its IPC
  *envelope* (length framing, the engine-assigned source endpoint, OS-channel identity) is
  transport-specific and never appears in replay files.

#### Process-host snapshots (deferred)

**Deferred with `SHARED`.** Everything in this part rests on the multi-reader `SHARED` engine view,
which is designed but not built in v1 (`design_engine_core.md` Appendix A). Until it exists,
a process host has **no snapshot access**. Its commands, events and lifecycle are
unaffected. The rules are kept because they are why `SHARED` would come back at all: no
other reader needs an engine view that a sandbox can read. How to revive it is open
([Q60](open_question.md#q60-reviving-the-shared-engine-view)).

**Snapshots over shared memory are an engine view with its blocks somewhere else.** There is no
broker, no mailbox protocol and no second copy. The sim thread's ordinary publish
(`design_engine_core.md` §3.2) writes into the child's segment, just as it writes into heap
blocks for every other reader. What the process boundary adds is a split mapping and these
rules:

| Aspect | Rule |
|---|---|
| Mode | **always `SHARED`.** A `PRIVATE` reader chooses the publisher's next block. `SHARED` exposes only bounded pin counts. The publisher may read them as availability, but never derives an address, index, epoch or loop bound from bytes the child can write |
| Mappings | the payload blocks and the published `(epoch,index)` word are read/write for the engine and read-only for the child. The four aligned block-state words sit in a separate region that is read/write for both, because pin and unpin are part of the reader protocol. Page separation is required. Permissions may not be weakened just to simplify the layout |
| What a scribble can do | corrupt the child's own read, or pin all candidate blocks so that the publisher skips only that engine view. The publisher claims only a state word exactly equal to zero, uses engine-private block addresses, cursor and epoch, and scans at most three candidates. Child bytes cannot steer core memory or delay the tick |
| Contents | the engine view's projection spec **is** the capability set, so the segment holds only the columns the mod may read, plus the `id` column. Filtering happens at projection time, where the schema is, not in a later copy step |
| Mod-side read | load the published word; atomically pin that block unless it is `WRITING`; reload and require the exact same published word; then copy the payload and unpin. Ordinary pointer or payload access starts only after this revalidation |
| ABI | a shared-memory engine view **is** a stable cross-boundary layout: fixed aligned `u32` state words and a `u64` published word at pinned literal offsets. It never uses `hardware_destructive_interference_size` or a native struct memcpy. Heap engine views use `std::atomic`. Shared memory uses platform interprocess-atomic wrappers over raw aligned integers (`__atomic_*` on supported Unix, `Interlocked*` on Windows). Startup refuses a platform on which these widths are not always lock-free, and the protocol is tested across processes |
| Reclamation | **only after process death is confirmed** (pidfd/waitpid). Never on a drop message or a deadline, since a live process may still be mid-copy |
| Size | **the segment is sized once, at the freeze, from the engine view's maximum rows** (`design_data_container.md` §5.1). Matched rows vary below that maximum, so the segment is never resized and the child never remaps |

**The segment never moves.** Every object type has a cap, and every engine view has a maximum
number of rows (`design_data_container.md` §2.2, §5.1). So the engine sizes the segment once
and commits its pages as matched rows reach them, the same way it treats a heap block
(`design_engine_core.md` §3.1). No publish waits for the child, and the child's read
protocol has no remap step.

So a slow, hung or hostile mod loses only the freshness of its own engine view. It cannot delay a
tick, corrupt the engine or affect another reader. The publisher does read the four fixed
state words. But it treats every nonzero or malformed value the same way, as "candidate
unavailable", and skips it after the bounded scan.

**Shutdown detachment never waits for the child.** At engine shutdown the publisher stops
writing, and the engine drops its own mapping. The child's mapped bytes stay readable but
frozen. Reclamation still waits for confirmed death, as above. So closing cannot hang on an
uncooperative or wedged mod process. This is the in-process detach rule
(`design_python_api.md` §7.2) carried across the process boundary. It lets the kill path
(§6) bound its own deadline, without a "give the child a moment to finish reading" step that
could never end.

#### Platform constraints for when process hosts are built

These constraints come from operating-system behaviour. None has been exercised yet. The
first applies to every process host, from the first one that ships. The others apply once
process-host engine views exist.

- **Create the containment unit before spawning the mod host.** On Windows, a process
  started through `Win32_Process.Create` escapes Job objects. On Linux, moving a process
  into a cgroup does not capture the descendants it already has. So the Job object or cgroup
  must exist before the mod host is spawned.
- **Seal the segment with `F_SEAL_FUTURE_WRITE`, never `F_SEAL_WRITE`** (Linux).
  `F_SEAL_WRITE` can only be applied after every existing shared writable mapping is
  unmapped. But the purpose of sealing here is that the engine keeps the only read/write
  mapping.
- **Confirm the child has the segment before publishing into it.** A file descriptor sent
  with `SCM_RIGHTS` is silently dropped when the control buffer is too short, or when the
  receiver is at `RLIMIT_NOFILE`. The sender still sees success. So bring-up needs an
  acknowledgement from the child to the engine that it received the descriptor *and* mapped
  it. Nothing is published into the segment before that.
- **A read-only mapping is not read-only** (Windows). `FILE_MAP_COPY` is reachable from a
  `FILE_MAP_READ` handle, so a child can privately write its nominally read-only payload.
  Engine bytes stay safe, and the state words are already treated only as bounded
  availability. But the pages the child dirties are charged to the paging file, and nothing
  bounds that exhaustion vector
  ([Q64](open_question.md#q64-bounding-a-process-hosts-copy-on-write-pages)).

## 5. Tier 3: core mods (native plugins)

A core mod is native code that registers systems into the deterministic scheduler. It runs
inside the determinism boundary, so it is trusted code, and it is audited twice.

**TL;DR**

- A core mod registers real systems into the deterministic scheduler. It lives inside the
  determinism boundary.
- Two separate audits: determinism (automatable, based on checksums) and trust (procedural,
  because native code in the process cannot be sandboxed).

### 5.1 Pattern: ABI and registration

A core mod exports one versioned C function, which the engine calls to register it. The
engine later calls an optional session-start callback, once the column set is closed.

```c
/* pseudo-code — the only exported symbol; versioned C ABI, never C++ */
sim_estab_mod_info_v1 *sim_estab_mod_register_v1(const sim_estab_host_v1 *host);

/* sim_estab_host_v1: engine ABI version, struct sizes, and the host services —
 *   register_column, resolve_column, get_row_set, get_column
 *     (design_data_container.md §7.3), core RNG stream derivation,
 *     checksum hook, log fn (buffered inside a phase — see below)
 * sim_estab_mod_info_v1: mod id/version, declared systems
 *     { name, read_set, write_set, phase, tick_fn },
 *   plus one optional callback:
 *     int (*on_session_start)(const sim_estab_session_v1 *s);   -- may be NULL
 *
 * sim_estab_session_v1: struct size, the frozen session identity hash, the tick rate,
 *   and this mod's RNG stream root (derived from the session seed, §5.2). Everything
 *   else a mod needs is reached through the host table it was handed at registration;
 *   the struct exists so that what is *session*-scoped is passed at session scope
 *   rather than captured from a registration-time global.
 */
```

Rules:

- **C ABI only.** C++ types, exceptions and templates never cross the plugin boundary. The
  C++ ABI is fragile across compilers, and module mangling makes it worse. No exception may
  propagate out of `tick_fn`. Errors are returned as codes, and the engine decides what to
  do (quarantine or abort).
- **A handshake comes before registration**: the engine ABI version, an echo of struct
  sizes, and a build-flags fingerprint (the subset that affects determinism). A mismatch
  refuses the load with a specific error.
- **Two stages, and they are not interchangeable.** Keeping them apart makes a column id
  mean the same thing to every mod (`design_data_container.md` §7.3):

  | Stage | Called | Allowed | Not allowed |
  |---|---|---|---|
  | registration | `sim_estab_mod_register_v1`, at load, in load order | `register_column`, declare systems | `resolve_column`: the column set is still open |
  | session start | `on_session_start`, at the freeze, in load order, on the **owner thread** (`design_engine_core.md` §2.4 step 7) | `resolve_column`, cache ids | `register_column`: the set is closed |

  A mod caches its ids in `on_session_start` and uses them for the rest of the session.
  `resolve_column` during registration returns an error, not a provisional id. An id that is
  right at registration and wrong at run time would be silent, deterministic corruption
  inside the determinism boundary. That is the one kind of failure nothing downstream can
  catch.
- **The engine never `dlclose`s a library, and registers each library at most once per
  process.** Three consequences follow:
  - `dlopen` runs static initializers and resolves `DT_NEEDED` *before* the handshake can
    reject anything. The handshake limits accidental ABI damage. It cannot limit what an
    initializer already did. That is a matter of trust (§5.4), not of mechanism.
  - The returned `sim_estab_mod_info_v1*` is **owned by the mod** and must stay valid for
    the life of the process. The engine neither frees it nor copies and frees it.
  - The engine remembers registered library paths across `Engine` instances. A second engine
    in the same process that would re-register the same library refuses with `ModLoadError`
    (`design_python_api.md` §8), instead of re-entering a mod's globals. This keeps "no hot
    reload" true under a retried load. So a **Tier 3 load failure is final for that mod in
    that process**. Tier 1 and Tier 2 failures can be recovered from by constructing a new
    engine (§6).
- Registered systems take their place in the **fixed system order** of the phase-structured
  scheduler (`design_engine_core.md` §4.1), just like built-in systems. There is no special
  scheduling path. A system declares an access kind **per column** it touches, from one
  closed set:

  | Kind | May read | May write |
  |---|---|---|
  | `read_full` | any row | nothing |
  | `read_local` | its own task range | nothing |
  | `write_local` | its own task range | its own task range, in place |
  | `write_staged` | any row | an engine-owned per-task buffer, merged in fixed order at commit |
  | `write_dbl` | any row of the front buffer | its own task range of the back buffer |

  The scheduler places the system in a phase where the invariant of `design_engine_core.md`
  §4.1 holds: one writer per column, and no cross-range read of a `write_local` column. A
  system whose declarations cannot coexist with either neighbour runs alone in its own
  phase. `write_dbl` is how a system correctly reads a column it also writes: the front
  buffer stays immutable for the phase. Each invocation then receives read *and* write spans
  computed from its declarations, together with the row set's live bitmap
  (`design_data_container.md` §7.3). The span is the permission, so a mod never has to work
  out which rows are safe to touch. Inside the span, a mod masks dead rows by the live
  bitmap; an append-only type has none.
- **Loading is split.** Python owns policy: discovery, the manifest, signature verification,
  user consent (§6). C++ owns mechanism: `dlopen`/`LoadLibrary`, the handshake,
  registration. Python hands the engine a vetted path; the engine never scans directories
  itself.
- Core mods load before the session starts and stay for the whole session. There is no hot
  reload, because it would invalidate replay identity.
- A core mod never reads snapshots. Inside the tick it reads and writes *live* columns
  through the host services table (`design_data_container.md` §7.3). A mod may ship an async
  peripheral half, such as a planner or a neural-net brain. That half is an ordinary
  peripheral with no special access: it reads snapshots and submits commands (§2).

### 5.2 The determinism contract

A core mod's `tick_fn` must obey the same rules as any core system (`design_engine_core.md`
§2). They are restated here as the contract the mod author signs.

- fixed-point and integer math only for state; no floats in anything that touches core
  state; no libm;
- time is the tick counter; no wall clock;
- randomness only from the RNG streams the host services table derives from the session
  seed;
- **diagnostics only through the host services table's `log fn`, which buffers inside a
  phase.** A mod must not call a logging library directly from `tick_fn`. Every call would
  take a lock inside the log core and serialize the parallel phase. That loses the
  throughput `design_engine_core.md` §4.1 exists for. Instead, the `log fn` appends to a
  **per-worker buffer**. The engine drains it at the phase boundary and emits the records
  normally (`design_patterns.md` §7). The merge uses the same fixed task order as the
  phase's `write_staged` buffers (`design_engine_core.md` §4.1), so it adds no
  synchronization of its own. Two properties follow. Log volume inside a phase is bounded by
  the buffer; what happens when a buffer fills is open
  ([Q38](open_question.md#q38-what-happens-when-a-per-worker-log-buffer-fills-during-a-phase)).
  And **records appear in a deterministic order**, however the workers were scheduled. So a
  log that differs between two runs of the same replay is a real signal, not scheduling
  noise. Records are never core state and never enter the checksum;
- persistent deterministic mod state lives only in registered dynamic columns, or in another
  engine-owned, checksummed state object (`design_data_container.md` §7.3). Undeclared
  private state that affects outcomes breaks the contract. Certification catches it and it
  is treated as a breach of trust, but nothing physically prevents it (§5.4);
- **reads and writes both stay inside the spans given to the invocation**, not just writes:
  - Reading outside the read span is as much a violation as writing outside the write span.
    A column another task is writing in place is not safe to read at all. The scheduler
    guarantees only that what it *gave* you is coherent.
  - Direct writes inside the write span are valid, because the phase invariant of
    `design_engine_core.md` §4.1 holds for every column in the phase.
  - Operations that may conflict, or that change structure, use `write_staged` (engine-owned
    buffers, committed in fixed order). They include claims that other tasks may also make,
    reductions whose result depends on order, creates and deletes, relationship rewiring,
    and writes to a destination found at run time.
  - **Structural results land in the next tick.** Creates and erases apply in the terminal
    commit. So an entity a mod creates has no row until then, and cannot be read back or
    written during the tick that created it. Act on it in the *next* tick;
- **scatter only with indices that are provably unique.** If a SIMD scatter's index vector
  repeats a row, which lane wins is unspecified. The result then varies with lane width,
  even on one thread. Uniqueness must follow from where the indices came from: a traversal
  of a `unique` link, or the distinct rows of the task range. It must never rest on
  an assumption about the data. Every index must also lie inside the write span
  (`design_data_container.md` §3). How mods traverse links is open
  ([Q58](open_question.md#q58-mod-link-traversal));
- **rows last for the tick; spans last for the invocation.** A row is a slot, and rows never
  move (`design_data_container.md` §2.2). But the tick's terminal commit is the only point
  where the live set changes, and after it a slot may hold a different entity. So a row is
  usable for the whole tick and no longer. A resolved column pointer or span is valid for
  **this invocation only**, because spans are recomputed for each phase and staged buffers
  merge at each boundary (`design_data_container.md` §7.3). So carrying a row into the next
  phase is legal; carrying the pointer you read it with is not. Anything a mod keeps past
  the tick, or puts into a command, is an id;
- no behaviour that depends on iteration order or addresses; no atomics that pick a winner;
  results independent of thread count.

### 5.3 Certification (the automatable audit)

The verification harness (`design_engine_core.md` §2.3) also certifies mods. It runs each
golden replay with the mod loaded, on one thread and on N, in Debug and in Release, and
requires identical full checksums: the whole world every tick, plus one hash per system.

```python
# pseudo-code — sim_estab.mod.certify
def certify(mod_path, golden_replays):
    for replay in golden_replays:
        runs = [run_headless(replay, mods=[mod_path], threads=t, build=b)
                for t in (1, N) for b in ("Debug", "Release")]
        require_identical(r.per_tick_checksums for r in runs)
```

- The per-system hashes of the full checksum make a desync attributable to the mod system
  that caused it. That holds in certification, and for a desync from the wild once its
  replay is re-run at the full level (`design_engine_core.md` §2.3).
- Certification compares Debug and Release runs. How that fits the engine build recorded in
  session identity is open
  ([Q23](open_question.md#q23-what-does-engine-build-mean-in-session-identity)).
- Certification runs headless through `Engine.step()`. That is one more reason the stepping
  primitive exists from the start.
- A mod that fails certification loads only with an explicit "uncertified" opt-in
  (`allow_uncertified` in `ModPolicy`), if at all. That is a policy decision.

### 5.4 Trust (the procedural audit and its limits)

Core mods cannot be contained, so trust rests on gatekeeping before load. This section lists
the gates and says plainly what they cannot do.

- `dlopen`ed code has **full process access. Native code in the process cannot be
  sandboxed.** Core mods are trusted code by definition. Every measure below is gatekeeping,
  not containment:
  - a signature or hash allowlist. Unsigned or unlisted mods are refused by default. Loading
    one anyway is an explicit act by the user, written as an `allowlist` entry in
    `ModPolicy` (§3.1). That decision is **recorded in policy, not answered in a dialog**.
    So it survives into CI, can be reviewed in a diff, and cannot be clicked through;
  - capabilities in the manifest are *declarative documentation* for core mods, since they
    cannot be enforced at run time. They are still required and still inspectable;
  - the version handshake (§5.1) prevents accidental ABI corruption, not malice.
- User-facing docs should say this plainly: installing a core mod means running a program.

## 6. Loading pipeline and main-loop integration

The host application loads mods in three calls: load, freeze, spawn. This section explains
that order, which stage each failure lands in, and the matching rule for shutdown: no
unloading function ends a session.

```python
# pseudo-code — sim_estab.mod, driven from the host app after Engine construction
def load_mods(engine, policy) -> ModBus:          # engine is `configuring`
    manifests = discover(policy.paths)                # find + parse mod.toml
    manifests = toposort(manifests)                   # deterministic order (§3)
    verify_signatures(manifests, policy)              # gatekeeping (§5.4)
    grants = resolve_grants(manifests, policy)        # manifest capabilities minus
                                                      #   policy.deny; never interactive

    bus = ModBus(manifests, grants)  # declares logic principals before the freeze
    engine.register_hosts(bus)      # lifetime handover, BEFORE any host exists:
                                    #   from here on, engine.close() owns shutdown
    for m in manifests:             # nothing here starts a session; any failure raises
        match m.tier:               #   ModLoadError and the engine is `load_failed`
            case "content": engine.load_content(m)                # hashed into session
            case "core":    engine.load_native_mod(m.vetted_path) # dlopen + handshake,
                                                                  #   register_column (§5.1)
            case "logic":   pass                    # manifest only — spawn comes later

    for m in [m for m in manifests if m.tier == "logic"]:
        # The freeze closes the engine view set. A process host's grant-projected
        # engine view is deferred with the SHARED mode (engine_core Appendix A);
        # a thread host may opt into a PRIVATE one.
        if policy.host_for(m) == "process":
            pass                              # no snapshot access in v1
        elif m.limits.private_view:
            engine.register_view(f"mod:{m.id}", columns=grants[m.id].columns,
                                 cadence=1)

    return bus                      # engine is STILL `configuring`. load_mods does not
                                    #   freeze, and cannot: the freeze is the caller's

# the caller, in this order and no other (design_python_api.md §2):
#   mods = load_mods(engine, policy)
#   engine.start_session()          # THE FREEZE (design_engine_core.md §2.4): column ids,
#                                   #   source ids + stable live endpoints, closed engine view set,
#                                   #   seed, identity, log file, tick-0 snapshot,
#                                   #   then each core mod's on_session_start
#                                   #   -> engine is `running`
#   mods.start()                    # AFTER the freeze: spawns one host per logic mod. A
#                                   #   host may read a snapshot in on_load, and its source
#                                   #   id already exists
```

- **Three stages, in a fixed order: load, freeze, spawn.** Load is reversible and has no
  observable effect. The freeze fixes session identity. Spawn starts the mod hosts. They are
  **three separate calls**. Folding the freeze into the loader would hide the one moment
  that fixes session identity inside a function named for loading. It would also drag
  spawning in with it, since spawning must follow a freeze. Three calls cost the caller two
  lines, and make each stage nameable, skippable and separately reportable. A replay, for
  example, simply does not call `mods.start()` (`design_python_api.md` §4.2).

  Each failure lands in one stage:

  | Fails during | Outcome |
  |---|---|
  | **policy work**: discovery, parsing, toposort, signature verification, capability resolution | `ModLoadError`, and the engine is still **`configuring`**. None of this touches the engine: the whole block runs before `register_hosts`, the boundary between policy work and engine load. So a bad path or a refused capability is *retryable*: fix the policy and call `load_mods` again |
  | **engine load**: from `register_hosts` on, meaning content, `dlopen` and handshake, column registration, and registration of mod-host engine views | `ModLoadError`, and the engine is `load_failed` (`design_python_api.md` §2). No identity was computed, no header written, no mod host spawned, and no log file named for a session. Recovery is a new `Engine`. A Tier 3 failure is also final for that mod in that process (§5.1) |
  | **freeze**: an identity mismatch against a loaded replay header, or a core mod failing `on_session_start` | `ReplayIdentityError` or `ModLoadError` from **`start_session()`**, not from `load_mods`. The same clean terminal state |
  | **spawn**: a mod host fails to start in `mods.start()` | **quarantine** (§4.2), not a load failure. The source id and the header entry already exist and stay correct, and the session runs without that mod's commands. How the mod leaves the gate is open ([Q9](open_question.md#q9-how-does-a-stopped-participant-leave-the-gate)) |

  The `register_hosts` boundary is why the pipeline puts *all* policy work first. The
  retryable failures are the ones a user can fix, and the ordering makes them retryable,
  with no rollback machinery.

  There is **no partly loaded session state** to define. The freeze writes the header, and
  the freeze runs only after every load has succeeded.
- **Mod hosts spawn after the freeze**, in `mods.start()`, not during the load loop. There
  are three independent reasons, each enough on its own:
  - `on_load` is where a mod reads its starting picture of the world (§4.1). That means
    `ctx.snapshot()`, which cannot work before a tick-0 snapshot exists.
  - Source ids are assigned at the freeze, from load order (`design_engine_core.md` §2.4
    step 3). So a failed spawn renumbers nobody.
  - The step most likely to fail then has the cheapest failure. A spawn failure is an
    already-defined state that can be attributed to one mod, not a question about identity.

  Making spawn a **separate call** adds a fourth reason: the caller can freeze without
  spawning, which is what a replay wants.
- **Every mod-host engine view is registered before the freeze.** Effective policy is already
  resolved, so the loader registers the requested `PRIVATE` engine view for a thread host that
  asked for one. A process host's engine view, projected from its column capabilities, will
  register here too once the `SHARED` mode returns (`design_engine_core.md` Appendix A).
  This is the only legal order, because freeze step 3a closes the engine view set. Spawn only opens
  the endpoint allocated earlier and retrieves the engine view registered earlier (a process host
  would map it). Spawn never adds either resource.
- **No fan-out can run during loading.** The bus fans out only in `bus.publish()`, which the
  host application calls in its loop. That happens after `load_mods` has returned and every
  inbox exists. Engine events emitted while loading wait in the event ring, which is bounded
  and lossless up to its size (`design_python_api.md` §7.3). They reach every subscriber on
  the first `bus.publish()`. Together with subscription by manifest (§4.1), this closes the
  gap between spawn and subscribe from both ends. A mod host is subscribed the moment it
  exists, and nothing is delivered until every mod host exists.
- The replay header records every loaded mod (§3). The freeze writes it.
- After loading, the main loop's only mod duty is `bus.publish(events)`
  (`design_python_api.md` §4.1). The bus carries events **outbound only**. There is no
  `drain_commands` and no command path through it (§3).
- **The engine owns mod host *lifetime*; `load_mods` owns mod host *construction*.**
  `register_hosts` is the handover. It happens before the first mod host is spawned, so a
  load that fails halfway still leaves every spawned mod host owned by something. The host
  application keeps the returned bus and drives `publish` with it, but it does not decide
  when mod hosts end. That is up to `Engine.close()` (`design_python_api.md` §2), the only
  object that knows the correct order relative to the sim thread and the native contexts.
  - **Why construction is not part of the `Engine` constructor**, which would look tidier.
    This pipeline is Python policy code that fails one mod at a time: `verify_signatures`
    rejects a single artifact, and `resolve_grants` narrows a single manifest. It reports
    those failures as `ModLoadError` naming the mod. The constructor cannot do that while it
    is bound by the C++ constructor-rollback rule (`design_patterns.md` §4). The pipeline
    also needs mod hashes to exist *before* session identity is fixed. That ordering has a
    name: loading happens in `configuring`, identity is computed at the freeze, and the
    constructor comes before both (`design_engine_core.md` §2.4). Registration gets the
    safety without tangling the order.
  - **`load_mods` does not call `start_session()`, and no loading function starts a
    session.** The rule holds across the design docs: loading mods, replays, content and
    native mods all leave the engine `configuring`. It is symmetric on the way down: **no
    unloading function ends a session**. `mods.stop()` leaves the session running, and
    `close()` ends it (`design_engine_core.md` §2.4, `design_python_api.md` §2). `load_mods`
    is the last step that knows the mod set is complete. That is not the same as knowing
    whether the caller is ready to fix session identity, and the freeze decides the latter.
- **Shutdown is `mods.stop()`, the first step of the shutdown sequence**
  (`design_python_api.md` §2). It runs with the GIL held, because `on_unload` is Python.
  **It does not end the session**: the session is still `running` when it returns, and
  `close()` ends it. This is the rule of `mods.start()` read backwards: no loading function
  starts a session, and no unloading function ends one. A caller may stop its mods and keep
  simulating. How the stopped mods leave the gate is open
  ([Q9](open_question.md#q9-how-does-a-stopped-participant-leave-the-gate)).
  1. Set `ctx.stopping`, and change the mod's endpoint lease from `OPEN` to `REVOKING`. Stop
     feeding its inbox and stop serving it snapshots. From then on a new endpoint lease
     returns `revoked`; a call that entered its lease before the change may finish. Wait for
     those running calls under `HostPolicy.shutdown_deadline`, then store `REVOKED`.
     `stopping` is advisory, so `False` followed by a revoked submit is legal (§4.1).
  2. Drain the inbox, then run `on_unload`, under `HostPolicy.shutdown_deadline`. **The mod
     host drains the inbox, not the engine.** `stopping` ends the wait loop, and the mod
     host then delivers what is already in its inbox before `on_unload` (§4.2). Revocation
     stops *new* events arriving. It does not discard events already delivered. Discarding
     them would break the reliable class's promise, "never silently lost at any stage"
     (`design_python_api.md` §7.3), at the moment a mod most needs the rejection it is
     holding.
  3. Join the mod host under the same `shutdown_deadline`. A process host that then has to
     be killed gets one more budget of the same length to confirm death
     (`design_python_api.md` §3). There is one number for every step, not three separately
     tunable ones, because nothing needs them to differ.

  **A pause stops all three budget clocks**, because a paused session is not advancing and
  nothing in it can be late (`design_engine_core.md` §3.3). Shutdown is not itself a pause.
  This matters only where the two overlap, but a pause taken mid-teardown must not turn a
  cooperative stop into a kill.

  **So `on_unload` cannot change the world.** Step 1 comes first so that a mod cannot inject
  state during teardown or extend shutdown by submitting. After the wait for running calls,
  every `ctx.submit*` returns `revoked`. It is a returned result, so a final flush is
  refused *visibly* instead of vanishing. `on_unload` releases the mod's own resources.

  When a deadline expires, the two kinds of mod host differ. This difference is a main
  reason process hosts exist:
  - **A process host can be killed.** Kill it and confirm death (`pidfd`/`waitpid`). Once
    process-host engine views exist, also unlink its engine view segment. Nothing survives.
  - **A thread host can only cooperate.** A thread stuck in native code cannot be killed
    safely in-process. So a hung thread host is logged and **abandoned**. An abandoned mod
    host may still hold an engine view block and still call `ctx.submit()`. So abandonment **moves
    the engine to `failed`** (`design_python_api.md` §2). The engine disarms instead of
    releasing, and ending the process is the only hard recovery. This is the same defect as
    a sim thread that will not join, one level down. It gets the same answer, not a logged
    warning that shutdown then walks past.

  A timeout in the wait for running submits on an endpoint lease follows the same split.
  Either kill and confirm the process host's supervisor, or abandon the in-process thread
  and put the engine in `failed`. In neither case is the ring reclaimed while its
  `active_submit` count may be nonzero. But the supervisor is an engine-side thread (§4.3)
  and cannot be killed, and `design_engine_core.md` §5.1 sends every endpoint-lease timeout
  to `failed`. Which rule applies is open
  ([Q46](open_question.md#q46-process-host-drain-timeout)).

## 7. Checklists

These checklists cover three common additions: a logic mod, a command type and a core mod.

New **logic mod** (for mod authors):

1. Write the manifest: tier `logic`, with the fewest command and event capabilities you
   need.
2. Implement the protocol (§4.1) against `ModContext` only.
3. Change the world only through `ctx.submit*(cmd, tick)`, naming the tick you are about to
   release. **Inspect both answers**: admission from the call, and the outcome from its
   handle. They always arrive, and they are the only report you get (`design_python_api.md`
   §7.1).
   - `queue_full`: you asked for more than your declared per-tick capacity. Declare more or
     submit less. It is not backpressure, and retrying the same tick will not help.
   - `over_margin` / `out_of_order`: you named the wrong tick.
   - `revoked`: your endpoint is going away; stop.
   - `invalid`: a bug in your mod.

   Prefer `submit_batch`. It is one crossing, it fixes the order of your commands within
   your source, and it is one IPC round trip instead of many. Reads are snapshots and events
   and may be stale; design for reaction latency.
4. **`on_unload` may not submit.** Your endpoint is revoked before it runs (§6).
5. **`ctx.snapshot()` is already a copy.** Hold it as long as you like, pass it between
   threads, carry it to the next tick. It has no size limit and costs other mods nothing.
   Declare a `PRIVATE` engine view only if you read a large slice every tick. Then read it inside
   `with ctx.view().take() as snap:` **in its own function**, so the array views die with
   the frame. `take()` is refused (`ViewBusyError`, retryable) while array views on the
   previous snapshot are alive (§4.1).
6. **Call `ctx.declare_ready(tick)` every tick.** You are a participant because you submit
   (§4.1); skip it and the session stops
   ([Q2](open_question.md#q2-must-every-mod-be-paced-every-tick)). Read outcomes only for
   ticks you have already released. The engine raises instead of letting you wait on a tick
   you are holding up.
7. **Drain your inbox.** A mod that lets it overflow is suspended: fed nothing, out of
   pacing, and resumed when it catches up (§4.2). Its endpoint and the commands already in
   it are kept, and nothing is unloaded. But the mod loses events: overflow drops
   best-effort events and merges coalescible ones, and a suspended mod is fed no events at
   all. How reliable events are protected meanwhile is open
   ([Q45](open_question.md#q45-protecting-reliable-events-in-mod-inboxes)). The mod stops
   affecting the world until it keeps up.
8. Stay independent of the mod host: no globals that the app also uses, no assumptions about
   the main thread. Tolerate dropped or merged events in the coalescible and best-effort
   classes. Reliable-class events are never silently lost (`design_python_api.md` §7.3).
9. Budget: your mod host's CPU time is accounted and visible. Heavy compute belongs in a
   process host, so declare one. Your speed also shows in the session's pacing: you hold
   each tick until you release it.

New **command type** (engine side; this is the mod security surface):

1. Define it once in the C++ command registry: packed layout, validation rules, application
   semantics.
2. Regenerate or check the Python builder (`command.py`) against the registry.
3. Decide which capability string covers it. By default no mod has it.
4. Validation must be total. Any byte pattern a mod can submit is either applied by the
   rules or rejected, never undefined behaviour. The same holds when a replay is loaded: an
   unknown command type or a newer schema version rejects the file, and is never skipped
   silently. Size limits are part of validation (`design_engine_core.md` §2.3).
5. It is automatically part of the canonical payload schema (the one-schema rule: replay and
   IPC). Bump the schema version; readers reject newer versions instead of guessing.

New **core mod** (author and engine):

1. Manifest tier `core`; document the intended systems and component sets.
2. Implement against the versioned C ABI (§5.1). No exceptions out, no C++ types across.
3. Sign the determinism contract (§5.2), then prove it: pass certification (§5.3) on the
   golden replay set.
4. Ship signed. Otherwise every user has to add you to their `ModPolicy` allowlist by hand
   (§3.1, §5.4), and most will not.

## References

- [openage pyinterface
  doc](https://github.com/SFTtech/openage/blob/master/doc/code/pyinterface.md): one-way
  boundary, GIL discipline, exception translation at every crossing (adopted); embedding
  machinery (not needed, since Python is the host).
- [openage architecture
  doc](https://github.com/SFTtech/openage/blob/master/doc/code/architecture.md): split
  between presenter and simulation threads; nyan-style content as data.
- `design_engine_core.md` §5 and §6: the command ring and snapshot artifacts, and the
  mind/body determinism split that logic mods instantiate.
- Factorio FFF and Gaffer determinism sources, collected in the References of
  `design_engine_core.md`.
