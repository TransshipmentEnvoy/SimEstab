# Engine Core Design

This document designs the SimEstab engine core. The engine core is a **deterministic simulation
kernel** that uses only integer and fixed-point arithmetic. Around it sit **peripheral
systems** that may use floats: rendering, data viz, AI and GPU compute. Peripherals affect
the engine core only by submitting commands. Both sides must be able to run in parallel.

Status: designed, not implemented. The built modules are `log`, `gpu`, `viz`, `util` and
`limits`. The `limits` module holds the decided values of `design_limits.md`, so the numbers
quoted here are real. Nothing in §3 to §6 exists yet.

Sections §3.2, §3.3, §3.4, §3.5, §5.1 and §5.2 are **normative**: an implementation may be
faster, but must not order operations differently.

Scope of v1. In v1, simplicity comes before progress guarantees:

- Engine views are `PRIVATE` only: one reader each. The multi-reader `SHARED` mode is designed but
  deferred to Appendix A until process mod hosts need snapshots; they are its only real
  user. It costs a full copy per reader, which dominates at the row counts
  `design_limits.md` §5 budgets for.
- An engine view filters rows (§3.4) as well as columns. Filtering rows is what makes 10⁷ live rows
  affordable; choosing columns alone cannot.
- The reader sends a small return header back to the publisher on every take (§3.5).
- Publishing may allocate memory. It never waits (§3.1).
- Every thread that parks does so on a mutex and a condition variable, under one wake-up
  rule (§3.3). No wake-up depends on `seq_cst` ordering.

Terms are defined in [glossary.md](glossary.md). Related designs: `design_python_api.md`
(Python lifecycle, options, main loop), `design_modding.md` (mod tiers) and
`design_data_container.md` (the world container, and snapshots in its §5.1).

---

## 1. The two-domain architecture

The engine has two domains: the deterministic engine core and the peripherals. This split is a
proven pattern. Photon Quantum calls it *Simulation vs View*, and RTS lockstep engines have
used it since Age of Empires.

```
                     commands (quantized)
   ┌────────────────────────────────────────────────┐
   │                                                ▼
┌──┴───────────────┐   snapshots / events   ┌──────────────────┐
│  Peripheral      │◀───────────────────────│  Deterministic   │
│  (float domain)  │    (one-way, copied)   │  engine core     │
│  render, viz,    │                        │  int/fixed-point │
│  AI, GPU compute │                        │  tick-based      │
└──────────────────┘                        └──────────────────┘
```

Two hard rules make the design work:

1. **Inbound: the only way to change engine core state is to submit a command** through the
   source's endpoint. Player input, AI decisions, network peers, scripts and debug tools all
   use the same path. Submission can fail, because the ring is bounded (§5.1). A command
   that was not admitted never enters the input stream, and the submitter learns this from
   the call's result, not from an event.

   The converse also holds: a command is only something a tick consumes to change engine core
   state. Anything that tunes the engine without touching engine core state (log level, pause, time
   scale) is a **control call**. It is never a command and never recorded
   (`design_python_api.md` §3).
2. **Outbound: the engine core publishes immutable snapshots, and events, that peripherals read.**
   Fixed-point values are converted to float once, at the snapshot boundary. Data never
   flows back except as commands.

What follows from these rules:

- **Replay** is the seed plus the command stream. (That is shorthand: the full replay
  artifact in §2.3 also carries the identities that rebuild the initial state.) Replays
  double as regression tests ("golden replays") and as desync diagnostics.
- **Lockstep networking is cheap on the wire.** Only commands cross it, so bandwidth follows
  the input rate, not the world size. That is what scales to the entity counts this design
  targets. It is not free inside the engine: each peer must be a participant, so that the
  tick waits for its input (§3.3), and commands from non-deterministic peripherals need an
  authority (§2). `design_multiplayer.md` covers the details.
- **AI may be non-deterministic**: float neural nets, GPU compute, even an LLM. Its
  decisions are quantized into commands and recorded, so the simulation can be reproduced
  from the record even if the decision-maker is not reproducible (transition determinism,
  §2).
- **A peripheral that crashes or lags cannot corrupt the simulation.** This holds in every
  session. Whether it can delay ticks depends on what it is. An observer cannot (§3.3). A
  participant can, because the engine core waits for it. A peripheral whose commands name their
  tick, such as a network peer, is a participant. One whose commands name no tick, such as
  a logic mod, or that only reads, is an observer, unless it registers as a participant
  explicitly with a paced engine view (§3.3).

Running peripherals on any thread is a backend capability, not an architectural guarantee.
Under the SDL3 backend, the window, swapchain acquisition and present belong to the main
thread (`design_python_api.md` §4.3). Peripheral preparation and compute may run on workers.

### 1.1 Cross-thread mechanisms: how this document specifies them

Every artifact above crosses a thread boundary, so each needs a precise specification. A
name such as "bounded MPSC" does not say what is atomic, in what order, or what happens when
two threads touch the same word at once.

**Every cross-thread mechanism states five things.** A mechanism missing any of them is
unspecified:

| Item | What it states |
|---|---|
| **Atomic objects** | each one it touches, with width, alignment, and whether it is always lock-free on the target |
| **Per-operation memory order** | the order on every atomic operation, and the release/acquire pair it forms, named explicitly |
| **Linearization point** | the one operation at which the mechanism's effect becomes true for every observer |
| **Progress class** | one of the terms below |
| **Failure and deadline behaviour** | what happens when the operation cannot complete: skip, promote, refuse, or a deadline that ends in a named state |

Progress classes, used in exactly this sense throughout:

- **Wait-free**: finishes in a bounded number of steps whatever other threads do. The bound
  is stated where the claim is made. Every steady-state mechanism below is wait-free.
- **Lock-free**: a retry always means another thread made progress. No v1 mechanism is
  lock-free without being wait-free. (Reading a `SHARED` engine view would be, in Appendix A.)
- **Short lock**: takes a mutex whose every critical section is a few loads and stores and
  at most one notify. Nobody waits, logs or calls out while holding it, so the wait is
  bounded by those instructions, not by another party. A gate declaration and the drain's
  side of the admission wait are short locks.
  - **Lock order.** Every engine mutex is a short lock: `gate.m` (§3.3), each endpoint's `m`
    (§5.1), the event side list's (§5.2) and the control block's (`design_python_api.md`
    §2). One nesting exists: `gate.m`, then one endpoint's `m`, in `revoke` (§5.1). No other
    two engine mutexes are held together, and nothing logs under one.
- **Bounded wait**: a thread may block on another party, and only in a declared wait. An
  undeclared wait is forbidden. A wait of the engine core follows three rules. It is the gate
  (§3.3) or one of the shutdown and revocation drains below. It is bounded by a declared
  deadline, or, for a pause or the event backlog, held deliberately with no deadline. Its
  expiry has a declared outcome. A caller's wait (admission, outcome, a step's ticks) is
  bounded by structure instead of a timer: it ends when the sim reaches a named tick, or
  when revocation ends it. A pause holds it as it holds the sim.
  - The main case is **pacing**. Under lockstep, every instance advances at the rate of the
    slowest one, because every peer must run the same command set (`design_multiplayer.md`
    §3.1). Refusing to wait there would cause a desync, not a smoother tick rate.
  - The engine core waits for a peripheral only because it is registered as a participant, never
    because of the artifacts it uses. Nothing makes the engine core wait implicitly.

**One idiom recurs: publication.** Fill the payload with ordinary writes. Then make it
reachable with a single release store or read-modify-write. The consumer's acquire on that
same object makes the payload visible. Never publish by setting a "ready" flag next to data
a consumer may already be reading: that is two operations, and two operations are not a
linearization point.

**No publish path reclaims memory.** No engine view uses hazard pointers, payload reference counts
or deferred reclamation. GCC 16 provides neither `<hazard_pointer>` nor `<rcu>`, so any such
scheme would have to be written here. Instead, engine view ownership moves by a single exchange,
and the permutation invariant of §3.2 makes block reuse safe without reclamation. (The
deferred `SHARED` mode uses a short-lived reader pin count instead, and pays a full copy per
reader; Appendix A.)

The operation lease and the endpoint lease are drains, not reclamation. Their small control
blocks outlive the resource they guard. At shutdown they either see zero active calls, or
disarm and leak the resource when the deadline passes (`design_python_api.md` §2, §4.3).

**The register.** Every cross-thread mechanism in the engine, and where its five items are
specified:

| Mechanism | Direction | Linearization point | Progress | Specified in |
|---|---|---|---|---|
| **Engine view** | sim → one reader; the reader's return header travels back on the same edge | publish: the release exchange of the engine view's control word (§3.2 (P2)); take: the release exchange at (T2), which also publishes the return header | publisher wait-free (1 exchange); reader wait-free (1 exchange), and a paced take adds a short lock to declare | §3.2, §3.5 |
| **Command ring** | one producer → sim | submit: the release store of the ring's write index; drain: the acquire load of it. A full ring parks the producer on the endpoint's mutex and condition variable, and the drain stores `read` under that mutex | drain wait-free (1 load), plus a short lock on each endpoint it frees space in; submit wait-free unless the ring is full, where a paced producer makes a **bounded wait** until the oldest tick in its ring has run and the host gets `queue_full` | §5.1 |
| **Command outcome** | sim → producer | the outcome's release publication at the end of the tick that consumed the command, made visible by (G5) | a paced reader **blocks, bounded** by the named tick running, on the tick-progress wait, and is refused outright if it still holds that tick; the host polls and never blocks | §5.1 |
| **Endpoint lease** | producer ↔ revoker | successful lease: the second acquire load of `access_state`; revocation: the `OPEN → REVOKING` exchange | submit side wait-free; revoker blocks under the shutdown deadline | §5.1 |
| **Event ring** | executor → the session's event drain | enqueue: the release store of the ring's write index | wait-free | §5.2 |
| **Event drain wake** | owner thread → sim | the drain's store of `read` under `gate.m`, with a notify only if the sim is parked on `EVENT_BACKLOG` | the drain takes one short lock per call; the sim's wait is the gate's | §5.2 |
| **The gate** | participants and host controls → sim | a participant's `ready_through` store, or a host-control store, each under `gate.m` (G3). The sim re-checks under the same mutex before it waits, and a writer notifies only a parked sim | **the sim blocks, bounded** by each blocker's declared policy; a writer takes a short lock | §3.3 |
| **Tick progress** | sim → threads waiting for a tick to run | the sim's store of `first_unexecuted` under `gate.m` after each tick's publish (G5) | the sim takes a short lock per tick; a waiter blocks until the tick runs, the owner thread in interruptible slices | §3.3 |
| **Operation lease** | any-thread API caller ↔ owner's `close()` | successful lease: the second acquire load of the access state; close: the `OPEN → CLOSING` CAS | call side wait-free; close drains under the shutdown deadline | `design_python_api.md` §2, §6 |
| **Async error handoff** | sim → owner thread | `error_state.store(READY, release)` after the winning producer fills the slot | wait-free; read at the next rendezvous | `design_python_api.md` §8 |
| **Staged join** | owner thread → sim | `stop_requested` stored under `gate.m`, with a notify if the sim is parked (§3.3); `std::thread::join` | owner blocks under a deadline | `design_python_api.md` §4.3 |

That is eleven mechanisms. Most of their steady-state work is a few atomic operations and,
on the sim, one short lock per tick. Four contain a declared wait, and all four are the same
kind of wait. The engine core waits for a participant; a producer waits for ring space; a producer
waits for a tick it has already released; and the owner thread waits for the ticks a step
granted. Every park in the engine follows the one wake-up rule of §3.3.

**Any new cross-thread mechanism gets a row here, with all five items, in the same change
that introduces it.** This mirrors `design_python_api.md` §9, where every new raise site
gets a named error type.

## 2. Determinism requirements for the engine core

The target is **bit-identical state** from the same seed and command stream: across runs,
thread counts and build types, and later across platforms. The cross-platform part depends
on the arithmetic and checksum rules of §2.1 and §2.3. As Gaffer On Games puts it: same
initial conditions plus same inputs give byte-for-byte identical checksums, not "close"
ones.

Two different properties are involved:

- **Transition determinism** (promised): the same initial conditions and the same canonical
  command stream produce identical engine core checksums. The engine core guarantees this, and
  replay checks it.
- **Session reproducibility** (not promised): re-running a live session's external producers
  (Python timing, garbage collection, mods, GPU and AI peripherals) gives the same command
  stream. It may not, because live timing changes when commands are submitted, which tick
  drains them, and which snapshot a mod saw. The record captures what actually happened.

For lockstep multiplayer, one authority must compute and broadcast the commands of
non-deterministic peripherals. If every peer generated its own, the peers would diverge
(§6). This applies only to non-deterministic peripherals. Player input needs only a submit
that names its tick, and the gate (§3.3; `design_multiplayer.md` §4).

### 2.1 Numbers: fixed-point on integers

The engine core uses fixed-point numbers stored in integers. Floats can be made deterministic
(Box2D 3.1 and later is deterministic across platforms using C17 floats). But that requires
forbidding `-ffast-math`, controlling FMA contraction (`-ffp-contract=off`), avoiding libm
with our own sin, cos and sqrt, and auditing every compiler upgrade. Integer arithmetic
avoids all of this: it is exact and identical on every conforming platform, including SIMD
lanes and, in principle, GPU integer units.

The choices:

- **Representation**: Qm.n in `int32_t` or `int64_t`. Q32.32 in `int64` suits world
  coordinates (Photon Quantum's `FP` is Q48.16 in `int64`). Multiplication needs 128-bit
  intermediates. `__int128` is a GCC extension, not a portable contract, so the
  specification is a 64×64 multiply-high pattern with a bit-exact result. `__int128` is
  allowed only as a GCC implementation of that same result.
- **Library**: candidates are [fpm](https://github.com/MikeLankamp/fpm) (header-only C++,
  fast), CNL (WG21/SG14, composable but few math functions),
  [Fixed64](https://github.com/SkynetNext/Fixed64) (`int64`-based, built for determinism)
  and libfixmath (C, Q16.16 only). We wrap one of them, or our own code, in a thin
  `fixed<Rep, Frac>` type, so that overflow, rounding and SIMD lowering are ours to define.
  **Those rules are a bit-level specification, not an implementation detail.** Rounding on
  multiply and shift, division (including division by zero) and overflow are defined exactly
  and versioned, so every build and platform computes the same bits. The library and the
  exact rules are open ([Q25](open_question.md#q25-the-fixed-specification)).
- **Overflow policy matters for parallelism.** Wrapping (two's-complement) addition is
  associative and commutative, so parallel sums give the same result in any order.
  Saturating addition is not associative, so parallel sums depend on order, which breaks
  determinism. Use wrapping arithmetic and wide enough types. Saturate only at well-defined
  sequential points. Wrapping comes from **unsigned representations and explicit
  bit-casts**, because signed overflow is undefined behaviour in C++. No compiler flag (such
  as `-fwrapv`) is part of this contract.
- Trigonometric and exponential functions use fixed-point CORDIC or lookup tables. The engine core
  never calls libm.

### 2.2 The usual desync sources (checklist)

These come from Factorio's Friday Facts, Gaffer On Games, and lockstep postmortems:

- **Iteration order**: never iterate hash maps or pointer-keyed containers to produce state
  changes. Order must come from stable ids, not addresses or insertion timing. Every object
  type is a pool whose rows never move. Rows are visited in slot order, which follows from
  the live bitmap, and the live bitmap is saved state (`design_data_container.md` §2.2;
  §4.1).
- **RNG**: a seeded PRNG owned by the engine core, such as PCG or xoshiro. Each system gets its own
  stream derived from the master seed, so systems cannot disturb each other's sequences.
  Peripherals never call engine core generators. The generator and how streams are derived are open
  ([Q26](open_question.md#q26-the-prng)).
- **Time**: the tick counter is the clock. The engine core uses no wall-clock time and no float
  `dt`. The counter's width and wrap rule are open
  ([Q28](open_question.md#q28-tick-counter-width-and-wrap)).
- **Uninitialized memory and padding**: zero-initialize state. Beware of hashing structs
  that contain padding.
- **Third-party code**: any library that touches engine core state must itself be deterministic.
  (ODE, for example, randomized its constraint order internally.) This is the main audit
  cost of an off-the-shelf ECS in the hot path.
- **Thread scheduling**: results must not depend on thread count or OS scheduling (§4).

### 2.3 Verification from day one

Determinism is checked from the start, before there is any simulation to test. This is the
natural companion of the "session management" work in M1.

- **Checksums of engine core state, at three levels.** One algorithm serves all three (§4.2,
  `design_limits.md` §6). They differ in how much state they cover and when they run:

  | Level | Covers | Runs |
  |---|---|---|
  | 1. **tick digest** | the commands drained this tick, in drain order, plus each object type's live count and extent | every tick, always. O(types + commands) |
  | 2. **rolling checksum** | each tick, the chunks whose index ≡ tick (mod N), of every object type; the whole world every N ticks | in multiplayer and while recording |
  | 3. **full checksum** | the whole world every tick, plus one hash per system | CI golden replays, certification and desync bisecting |

  The tick digest catches a divergent command stream or live set at once. The rolling
  checksum catches a divergent value within N ticks, at a flat cost of 1/N of a full pass
  per tick. The period N is open
  ([Q67](open_question.md#q67-the-rolling-checksum-period-and-the-tick-digest)). The full
  checksum costs a full pass per tick, which is why it runs only where a desync must be
  pinned to one tick. Its per-system hashes find which system desynced; the Unity DOTS
  lockstep write-up found them indispensable. A system's hash covers the columns it
  declared as writes, hashed when its phase closes.

  The checksum input is canonical, never raw memory: live semantic values in fixed
  little-endian byte order, without padding, spare capacity, dead rows or SIMD tail lanes
  (`design_data_container.md` §3). Each row's id is mixed into its contribution, so the
  reduction does not depend on row order (§4.2, `design_limits.md` §6). The id contains the
  slot, so the layout is covered too. Each object type's generation column, live and retired
  bitmaps and extent decide the ids of future creates, so they are part of the input.
  Derived columns (`design_data_container.md` §2.1) are left out of the tick digest and the
  rolling checksum; the full checksum rebuilds them and compares. Exactly which state must be
  hashed, and the bit-level algorithm, are open
  ([Q27](open_question.md#q27-the-checksums-exact-algorithm-and-inputs)).
- **Replay harness**: record the seed and the commands. In CI, a replay must produce the
  same final checksum with 1 thread and N threads, in Debug and Release, and later on Linux
  and Windows.
- **The replay artifact.** "Seed plus commands" is shorthand. The full replay input is:
  - the canonical initial-state identity: initial state and content tables, either embedded
    or referenced as content-addressed artifacts (the header states which, for each
    artifact);
  - deterministic config, such as the tick rate;
  - engine build identity. The checksum and PRNG algorithms are part of it and are not
    versioned separately. What "engine build" should mean is open
    ([Q23](open_question.md#q23-what-does-engine-build-mean-in-session-identity));
  - schema, content and mod identities (`design_data_container.md` §7.4, `design_modding.md`
    §3);
  - the cap of every object type, declared or the default of 2²⁴
    (`design_data_container.md` §2.2). A create at a cap is a deterministic rejection, so a
    different cap changes what the simulation does, and the header must catch it;
  - the source-id map, from the engine-assigned `u32` to the source's name. Commands carry
    only the number, so without the map a recorded stream cannot be attributed to a named
    mod;
  - the seed;
  - the canonical command stream, recorded as it is consumed (§5).

  Control calls (log level, pause, time scale) stay out of the replay header. During a live
  run they can still change which new peripheral commands are generated (session
  reproducibility, §2).
- **Command payload encoding**: one canonical payload schema. It uses explicit little-endian
  byte order, fixed integer widths, decoding without alignment assumptions, and total
  validation: every byte pattern is either valid or rejected, on live submit and on replay
  load. An unknown command type or a newer schema version rejects the file or frame; nothing
  is skipped silently. Size limits are enforced. Transport envelopes differ by use: replay
  chunks add indexes and checksums, and IPC frames add length framing and the
  engine-assigned source endpoint (`design_modding.md` §3, §4.3). Native structs are never
  copied raw across either boundary.

### 2.4 Session lifecycle: configure → freeze → run

A session has two states and one transition between them. Everything §2.3 records (content
tables, mod-registered columns, the mod set, the seed) is assembled after the engine
acquires its resources. Everything that uses it needs a set that can no longer change. So
one instant matters, and this section defines it: the **freeze**, where session identity is
computed and the world stops being configurable. The Python state machine is in
`design_python_api.md` §2. Who calls the freeze, and why loading comes before it, is in
`design_modding.md` §6.

- **`configuring`**: resources are acquired, but no world is running. Allowed: load content,
  load and register engine core mods (including `register_column`, `design_data_container.md`
  §7.3), register mod hosts, load a replay artifact, and control calls (§1). Not allowed:
  ticks, command submission, engine view reads and checksums. Each of those is defined in terms of
  a session, and there is none yet.
- **The freeze**: one call on the owner thread, running the ordered steps below. **No
  loading function performs the freeze.** Loading content, loading a replay, loading native
  mods and registering mod hosts all leave the engine `configuring`. The caller always
  starts the session with a separate call. The reverse also holds: **no unloading function
  ends a session.** Stopping mod hosts, draining inboxes and running `on_unload` leave the
  session running; `close()` ends it (`design_modding.md` §6, `design_python_api.md` §2). If
  a loader froze as a side effect, session identity would depend on which loader ran and in
  what order. It would also drag every step that must follow the freeze, starting with
  spawning mod hosts, into the loader.
- **`running`**: the world exists and its identity is fixed. Nothing in §2.3's list may
  change until the engine closes: no schema change, no mod load, no reseed, at any tier
  (`design_data_container.md` §7.4, `design_modding.md` §5.1).

**The freeze, in order.** Each step uses the output of the one before. **Every step that can
fail comes before every step with an observable side effect.** Step 7 is the last that can
fail, and step 8 is the first that is observable. That is what makes a failed freeze a clean
terminal state instead of a half-started session.

1. **Close dynamic column registration, and with it the row width.** The registered set is
   final, and column ids are assigned from it (`design_data_container.md` §7.3). A later
   `register_column` is an error, not a resize.
2. **Close every object type's cap and reserve its storage.** Caps come from the
   `[[=cap(N)]]` annotation and from `EngineConfig.entity_capacity`; a type with neither
   gets 2²⁴. After this step no cap can change (`design_data_container.md` §2.2). The step reserves
   address space for `cap × element size` for every column. Pages are committed later, as
   each pool's extent grows. A reservation can fail, which is one reason this step comes
   before every observable one. It comes after step 1 because the row width is not final
   until dynamic columns are closed.
3. **Assign source ids** and build the map between names and ids. Ids are `u32`: the host is
   0, and mods are numbered in their deterministic load order (`design_modding.md` §3). Ids
   are assigned here and bound to an endpoint when one is created. That split keeps them
   independent of whether a Tier 2 mod host ever starts. A replay starts none
   (`design_python_api.md` §4.2) and still needs the map to attribute the recorded stream,
   and a host that fails to start renumbers nobody.

   3a. **Close the engine view and participant registries** (§3.1, §3.3). Each engine view's declarations
   are now final: its projection spec, whether it carries the `id` column (§3.4), its row
   predicate kind, its cadence, and whether it is paced. The participant set,
   and each participant's deadline and expiry policy, are final too. Both registries close
   here for the same reason: they set the per-tick cost. Publish cost is a sum over engine views,
   and latency is the largest deadline over participants. A cost that can change during a
   session cannot be planned for. The predicate *kind* closes here for the same reason, but
   its *parameters* do not. Parameters change how many rows match without changing the shape
   of the cost, so they arrive with each publish through the return header (§3.5). There is
   no engine view mode or placement to close: every v1 engine view is `PRIVATE` and heap-allocated, and
   both return with the `SHARED` mode (Appendix A). The host is not a participant (§3.3), so
   in the simplest session the participant set is empty. This step comes after step 3
   because a participant is identified by its source id.
4. **Resolve the seed**: from config, or generated and then recorded, or from the replay
   header if a replay is loaded.
5. **Compute session identity.** The components are exactly §2.3's replay artifact.
6. **Check against the replay header**, if a replay is loaded. The check is component by
   component, and a mismatch rejects the artifact, naming the first component that differs.
   This is why the engine build and schema identity fields exist; without this check they
   would record something nothing reads. The seed cannot fail here, because step 4 took it
   from the header. So this step really checks what the runner supplied: the build, schema,
   content and mod set.
7. **Compute the phase cut** from the now-closed system list (§4.1). Then **call each engine core
   mod's session-start entry point**, in load order. This is the first point at which
   `resolve_column` returns a stable id (`design_data_container.md` §7.3). It is the last
   step that may fail. It runs before any side effect because it is the one step that runs
   third-party code. A mod that cannot resolve what it registered must abort a session
   nobody has seen yet, not one already publishing snapshots under a named log file.
8. **Open the per-session log sink**, named from the identity that now exists
   (`design_logging.md` §2).
9. **Publish tick 0 into every registered engine view** and emit `session.started`. This marks the
   start, before tick 1. The owner thread publishes it, because no sim thread exists yet
   (`run_sim_async` is not allowed in `configuring`). It is the only publish not made by the
   tick loop, and §3.2 needs no special case for it. Reading an engine view is not allowed in
   `configuring`, so no reader can see an engine view that has never been published. That state is
   unreachable rather than defined, and the protocol needs no sentinel value.

Steps 1 to 7 compute and allocate over state that is already loaded, plus one call into code
that is already loaded. They can fail, and a failure leaves nothing observable: no identity
is published, no host is started, no log file carries the session's name. That makes a
failed load a clean terminal state (`design_modding.md` §6). It is also why the freeze, not
the loader, writes the header. A header listing "every loaded mod" cannot describe a load
that did not finish, because it is only written when one does.

**"Observable" means observable through the engine**: a published identity, a started host,
a named log file, a snapshot someone can take. Step 7 runs third-party code, which may do
anything to the filesystem. It is still the last step that can fail rather than the first
observable one, because nothing it does is reachable through an engine interface. A mod that
writes a file during session start and then fails leaves a file behind, not a session. The
engine's promise covers its own interfaces, not every byte on the machine.

## 3. Time: fixed tick, latest snapshot

The engine core runs in fixed ticks and the renderer shows the latest snapshot. This is the
standard *Fix Your Timestep* structure (Gaffer On Games), without interpolation.

- **The engine core advances in fixed ticks**, 30 per second by default (`design_limits.md` §1).
  Real time accumulates, and the loop runs whole ticks from it.
- **The renderer runs at its own rate and draws the latest published snapshot as it is.**
  There is **no interpolation between snapshots**. World motion updates at the tick rate,
  which suits a grid-based, slow-moving establishment sim. In exchange, the renderer holds
  one snapshot instead of two, there is no interpolation factor to pass around, and
  interpolation's extra tick of visual latency is gone. Camera and UI motion stay smooth at
  the frame rate, because they are float-domain peripheral state, not snapshot data. The
  snapshot format itself does not change, so interpolation could be added later inside the
  viz layer alone if a high frame-to-tick ratio made stepping visible.

### 3.1 Snapshot publication: the engine view

**A snapshot reaches a peripheral through an engine view.** It is the only way state leaves the
engine core: one mechanism and one data structure.

An engine view is **three payload blocks plus one published word**. The engine core writes only a
block it owns exclusively, then publishes it with one atomic exchange. Nothing on the publish path
is reference-counted or reclaimed.

**An engine view is `PRIVATE`**: it has exactly one reader, and that reader is trusted.

| Property | Value |
|---|---|
| Readers | exactly one, trusted |
| Blocks | **3** |
| Take | one `exchange`: the reader hands back its old block and receives the newest |
| Retention | the reader keeps its block **as long as it likes, without copying**; the publisher provably never touches it |
| Reader progress | wait-free |
| Who may own one | engine code, the host, a thread-host mod |

Three blocks is derived, not picked. The reader's index, the publisher's index and the index
in the control word always form a permutation of `{0, 1, 2}` (§3.2). So one block is held by
the reader, one is being written, and one is the newest published block not yet taken.

**A multi-reader `SHARED` mode is specified in Appendix A and is not built in v1.** It costs
a full copy per reader, which is the largest cost in the whole design at the row counts
`design_limits.md` §5 budgets for. Appendix A explains why the copy is unavoidable. The only
user that really needs the mode is a process mod host, which cannot be given a `PRIVATE`
engine view (see the rules below, and `design_modding.md` §4.3). `SHARED` returns when process
hosts do.

**An engine view is registered before the freeze, not requested on the fly.** The freeze closes the
registry (§2.4 step 3a). Each engine view declares:

| Declared | Why it is declared rather than inferred |
|---|---|
| **projection spec**: which columns | The projection is what the payload is. Declaring the columns per reader makes publish cost knowable at construction |
| **row predicate kind**: which rows | This decides whether 10⁷ live rows are affordable at all (§3.4). The kind is fixed here so the cost model is closed; its parameters change per publish through the return header (§3.5) |
| **cadence**: publish every `k` ticks | An analytics engine view at `k = 30` costs a thirtieth of a render engine view. Cadence is per engine view; there is no global cadence setting |
| **paced**, with a deadline | A reader that must see every tick registers a paced engine view and becomes a participant (§3.3) |
| **identity**: whether the `id` column is projected | On unless declined. A reader that never needs to know which entity a row is, such as a renderer, declares `identity=False` and saves 8 bytes per matched row (§3.4) |

**A paced engine view's take is its declaration.** Taking the block of tick `t` declares the
reader ready through `t + k`, where `k` is the engine view's cadence: up to and including its next
publish. `take()` makes that store itself, under the gate mutex, with a notify if the sim is
parked (§3.3). So the sim may run to the next publish, and no further, before the reader
takes again; every publish is seen. Until the first take, the reader is ready through tick
0, which the freeze publishes (§2.4 step 9). A take that finds nothing new declares nothing.
A paced engine view ignores cadence hints (§3.5): its cadence is part of its declaration.

**A paced engine view is a participant of its own**, registered with the engine view (§3.3). Its
take is that participant's only declaration. No reader has another: a logic mod never
declares ready, because its commands name no tick (`design_modding.md` §3). So a mod is at
most one participant, and no participant has two writers.

**A paced reader takes between steps, never inside one.** A thread inside `step(n)` cannot
take, whether it runs the ticks itself or waits for the sim thread to run them (§3.3). So
when that thread is also the engine view's reader, a step that runs past the engine view's
next publish leaves the gate waiting for a take that cannot come. The gate waits out the
engine view's deadline and then applies its expiry policy, and under `FAIL` the session ends
in `failed`. After taking the block of tick `t`, such a reader steps to `t + k` at most, then
takes again. A reader on another thread has no such limit. The engine does not detect the
case: an engine view's reader is whichever thread takes it (`design_python_api.md` §6).

**Placement**, heap or shared memory, is not a v1 declaration. Every engine view is heap-allocated
engine memory. Shared-memory placement comes back with `SHARED` and its split-permission
interprocess ABI (Appendix A, `design_modding.md` §4.3).

Three rules follow from this:

- **A process-host mod gets no engine view in v1.** A `PRIVATE` publisher uses the index the reader
  wrote (§3.2), so a hostile reader could steer a payload address. That cannot be handed to
  a sandbox. `SHARED` can face an untrusted reader, because the reader writes only
  block-state words that the publisher reads as availability and never dereferences
  (Appendix A, `design_modding.md` §4.3). A thread-host mod is trusted and registers
  normally.
- **A snapshot nobody registered for is made on request.** `engine.snapshot()` needs no
  engine view (`design_python_api.md` §7.2). The owner thread sees the world without
  registering anything, and nothing is published for a reader who never asks. Below.
- **The number of engine views is capped** (`design_limits.md` §2.3), because publish cost is a sum
  over them. The cap bounds the number of terms in that sum. Projection width and matched
  rows are measured, not capped.

**Publish (sim side)**, once per tick, after the terminal commit:

- For each engine view due this tick by its cadence, the sim does four things. It reads the
  reader's return header (§3.5) and evaluates the row predicate to a mask. It gathers the
  declared columns of the matched rows into the writable block. Then it publishes the block
  with one exchange. §3.4 covers the predicate and §3.2 the mechanism. A tick under a
  `step()` grant publishes exactly the same engine views as any other tick: those due by
  their cadence (`design_python_api.md` §4.3).
- **Publishing never waits.** It may commit memory: a block commits pages as this tick's
  matched rows reach them (see block memory below).
- **A `PRIVATE` engine view always has a writable block**, so a due publish always happens and is
  never skipped. v1 loses no samples; only `SHARED` can skip (Appendix A).

**Take (reader side):**

- One exchange returns the newest block and hands back the old one. If nothing new was
  published, the reader keeps what it has. Render then shows the same tick again if the
  camera moved, and may skip tick-derived CPU work. This only allows skipping recomputation.
  It never allows assuming that GPU work submitted earlier has finished; that remains a
  fence question (`design_data_container.md` §5).
- **The reader writes its return header before it takes** (§3.5). The exchange that hands
  back the block also publishes those writes to the publisher, so the return channel needs
  no second mechanism and no second ordering argument.
- **A take invalidates the previous block**, and this is the one sharp edge of the
  mechanism: the block handed back is the next one the publisher writes. So **a take is
  refused while any array view derived from the previous block is still alive**
  (`design_python_api.md` §7.2). Nothing is released without its holder knowing. The caller
  drops its array views and takes again, or copies. Only the caller's own array views can
  cause the refusal; no other reader can.
- If the sim is faster than the reader, unread publishes are overwritten: an engine view is sampled,
  not consumed. If the sim is slower, the reader keeps what it has. Neither shows up as a
  failure. That is why the return header carries `last_consumed_tick` (§3.5): it is the only
  way to measure a lagging reader.
- There is deliberately no "one frame per tick" coupling through a condition variable or
  semaphore. A reader that must receive every tick registers a paced engine view, becomes a
  participant (§3.3), and pays for that guarantee at the gate. A reader that only wants to
  shed load sends a cadence hint (§3.5) and stays out of the gate.

**On-demand snapshots.** `snapshot()` copies the world into one engine-owned buffer. It is
an owner-thread call, made while no tick can run. It touches no engine view and needs no
cross-thread mechanism.

- **Who calls it, and when.** The owner thread, and only while the session is quiescent: no
  sim thread runs, or it is paused with no step in flight, or the session has stopped. That
  is `checksum()`'s contract (`design_python_api.md` §6). With no sim thread, only the
  owner thread can run a tick, so none runs while it is inside `snapshot()`. Off the owner
  thread the call raises `EngineThreadError`. While the sim thread runs it raises
  `EngineStateError`.
- **What it copies.** Every `[[=viz]]` column of every live row, with the `id` column,
  converted to float as a publish converts (§3.4, `design_data_container.md` §5.1).
- **One pass.** A call is one mask, scan and gather over engine core state: the publish kernel of
  `design_data_container.md` §5.1, run once. Nothing is copied a second time.
- **One buffer, reused.** The **snapshot buffer** is reserved at the freeze, as an engine
  view block is (below): address space for every `[[=viz]]` column at each object type's
  cap, with pages committed as live rows reach them. Every call refills it. So at most one
  on-demand snapshot exists, its memory stays warm, and no call allocates.
- **A call is refused while array views of the previous result are alive**, as a take is
  (above, `design_python_api.md` §7.2). Refilling the buffer would change memory a caller
  still reads. The caller drops its array views and calls again, or copies what it keeps.
- **Cost.** One projection per call, on the owner thread. No tick runs meanwhile, so the
  tick budget below does not carry it. A session nobody inspects pays only the address
  space.
- **Other callers.** A snapshot on request from another thread, or from the owner thread
  while the sim thread runs, needs a request to the executor and a way for several readers
  to share one result. That design is open, and `checksum()` shares both the contract and
  the question
  ([Q84](open_question.md#q84-how-are-snapshot-and-checksum-opened-to-other-threads)).
  Until it exists, a reader that must see the world while ticks run registers an engine
  view.

**CPU/GPU lifetime boundary: the GPU never reads engine view memory.** `render()` copies the
`[[=gpu]]` columns on the CPU into a **staging transfer buffer**, then unmaps it. Under the
SDL3 backend the staging buffer is mapped and unmapped every frame, because SDL does not
allow upload commands to be recorded while a transfer buffer is mapped. Destination buffers
are cycled and rewritten whole, one per column. Each keeps a `last_uploaded_snapshot_tick`,
advanced when its fence signals and used only to skip an upload when the tick has not
advanced. Only uploads whose fence has completed are presented. The full contract is in
`design_data_container.md` §5.

**Block memory is reserved up front.** An engine view's matched rows can change on every
publish, up to the engine view's maximum rows: its object type's cap, unless the engine view declares
fewer (`design_data_container.md` §5.1). Each block reserves address space for that maximum
at the freeze and commits pages as matched rows reach them. A commit happens after the
exchange and before the fill, when the publisher's block is its own. So a block never moves,
and the bounded atomic step never allocates.

**No block is freed during a session.** Committing is tick-budget work done before the
release publication; it is never part of the bounded atomic step. Blocks never return pages
to the OS. A freshly committed page costs one page fault on first fill, so reusing a warm
block is what keeps publish time steady. Returning pages and committing them again would
make that jitter worse.

**The number of blocks is fixed.** A `PRIVATE` engine view always has a writable block, so it never
needs a fourth. Under `SHARED`, the alternative to skipping a publish is adding blocks
(Appendix A); that choice belongs to the `SHARED` mode
([Q60](open_question.md#q60-reviving-the-shared-engine-view)).

**Cost, and where it is charged.** Per-engine-view projection costs more than one shared projection
would, in one way:

- Publish cost per due engine view is `scan(predicate inputs × live chunks) + gather(matched rows ×
  spec)`, not one full copy. Two engine views over the same columns cost twice. That is the price
  of keeping blocks without copying, for both.
- **The row predicate shrinks the gather term.** A render engine view over 10⁷ live rows whose
  `FRUSTUM` matches 10⁴ gathers 10⁴ rows. Without a predicate, the same engine view is 320 MB per
  publish at a 32-byte spec (`design_limits.md` §5), and no choice of columns makes that
  affordable. That is why the row filter is a declaration and not an optimization.
- **The scan term remains, and it only reads.** A spatial predicate reads its input columns
  for every live row, sequentially and without writing: about 12 bytes per row for a
  position test. (Whether 12 bytes is right for Q32.32 coordinates is open:
  [Q61](open_question.md#q61-what-row-width-does-the-predicate-scan-assume).) This is the
  real floor of the design. The two ways below it are a spatial index behind the predicate
  kind (§3.4), and running scan and gather on the worker pool. A narrower spec does not
  help.
- The projection spec itself is the other lever. A render engine view takes the `[[=viz]]` columns,
  a GUI engine view takes two, an analytics engine view takes five at `k = 30`. With declared column
  lists, total bytes are normally below one undeclared full projection. That is why the
  column list is a declaration and not a default.
- **There is no maximum width per row** (`design_limits.md` §5). A per-row cap would bound
  the wrong thing: cost is width times matched rows. Width is not capped, and matched rows
  are bounded only by the engine view's maximum rows (`design_data_container.md` §5.1).
- **Cost is measured, not capped, and crossing the threshold is diagnosed.** Publish
  duration, publish bytes per second and matched rows are first-class metrics, per engine view and
  in total. **The threshold is 4 GB/s, checked per engine view, not against the total**
  (`design_limits.md` §5). It is a bandwidth, so it does not depend on tick rate or cadence.
  It is per engine view because every remedy here (narrow the spec, tighten the predicate, lower
  the cadence) changes one engine view's declaration, so a warning must name one engine view. Crossing it
  logs a warning naming the measured rate and the engine view. It never rejects anything, and the
  simulation does not change. The total is still reported, but the threshold is not checked
  against it.
- If measurements exceed the budget, the safe offload options, in order of preference:
  1. scan and gather on the worker pool. This is safe because the world does not change
     between the terminal commit and the next tick, and order is kept because the gather
     positions come from a prefix sum (§3.4);
  2. a bit-exact copy of engine core data on the sim thread, then fixed-to-float conversion on
     another thread.

  Until profiling justifies one of these, the publisher does the projection. Publishing only
  changed data (partial or dirty publish) is ruled out (`design_data_container.md` §5); the
  row predicate is a different axis and does not change that.

**Metrics.** Per engine view: publish duration, publish bytes per second, matched rows, last
published tick, and **consumer lag**: last published tick minus `last_consumed_tick`, read
from the return header (§3.5). Without lag, a `PRIVATE` reader that falls behind would be
invisible, because the publisher simply overwrites and nothing fails. Sustained lag
identifies a slow reader.

**Commands per tick is a first-class metric too**, in total and per source. A tick runs
everything stamped for it (§5.1), so nothing below `C` limits the drain cost. It is whatever
the session's producers submit. A session approaching that ceiling should show as a rising
number long before it shows as a frame-time problem. The per-source figure names the
endpoint to narrow, just as the per-engine-view figure does for publish.

**The tick budget is the wall-clock length of one tick**: 33.3 ms at 30 Hz
(`design_limits.md` §1). These costs are charged against it:

| Charge | Shape | Bounded by |
|---|---|---|
| Publish | `Σ over due engine views (scan over live chunks + gather over matched rows × spec)` | **nothing, by design**. Measured, and each term is checked against 4 GB/s **for that engine view** (above; `design_limits.md` §5). The engine view cap bounds the number of terms; each term's gather is bounded by its row predicate (§3.4); its scan stays proportional to the live chunks |
| Terminal commit | O(erases + creates + links into erased entities), sequential | the tick's staged creates and erases. No step is O(rows): rows never move (`design_data_container.md` §2.2) |
| Checksum | tick digest: O(types + commands), every tick. Rolling checksum: 1/N of a full pass per tick, when enabled. Full checksum: a full pass per tick | the level in use (§2.3). The full checksum runs only in CI, certification and desync bisecting, never in a shipped session |
| Upload bytes per frame | the render engine view's spec over its matched rows, whole columns | same treatment as publish |
| Command drain and execution | O(commands stamped for this tick) | `C = Σ capacity(endpoint)` over the registered endpoints (§5.1, `design_limits.md` §2). Only a session that registers that many endpoints, each at full capacity, reaches it |
| System execution | the actual simulation work | not yet measured |

**Time waiting at the gate is not charged.** Time at the gate (§3.3) is pacing, not work:
the engine core is deliberately not running. Counting it would make a healthy lockstep session look
like an overrun.

### 3.2 Engine view protocol (normative)

§3.1 fixes the design. This section fixes the mechanism, with the five items of §1.1. It is
normative: an implementation may be faster, but must not order operations differently.

**State.** The one atomic field is naturally aligned and always lock-free on the supported
x86-64 baseline: the `PRIVATE` word is 4 bytes. No 16-byte atomic and no tagged pointer is
allowed.

```
struct EngineView {
    Block*            block[3];
    std::atomic<u32>  word;
    ReturnHeader      ret[3];        // §3.5; consumer-written, parallel to block[]
    u32               write_index;   // publisher-private
    u32               read_index;    // reader-private
};
```

The private indices are plain variables, each touched only by its owner. `ret[]` is plain
memory on both sides. It is ordered by the same exchange that transfers the block, never by
an atomic of its own (§3.5).

`word` is `(index << 1) | dirty`.

```
publish:  fill *block[write_index]                               # (P1) plain; the block is private
          prev = word.exchange((write_index << 1) | 1, acq_rel)  # (P2) LINEARIZATION POINT
          write_index = prev >> 1

take:     if ((word.load(relaxed) & 1) == 0: return HELD         # (T1) nothing new
          prev = word.exchange(read_index << 1, acq_rel)         # (T2) LINEARIZATION POINT
          read_index = prev >> 1
```

**Theorem.** `{write_index, read_index, word >> 1}` is a permutation of `{0, 1, 2}` at every
instant. So `write_index != read_index` always, and the publisher never writes the block the
reader holds.

*Proof:* each operation is one exchange that swaps a private index with the shared one,
which is a transposition. The initial assignment is a permutation, and transpositions keep
it one. No interleaving needs checking, because each private index is written only by its
own side, and only after its own exchange returns.

The ordering works in both directions. The acquire half of (T2) sees the publisher's
released payload. The release half of (T2) pairs with the acquire half of the next (P2), so
all the reader's loads happen before the publisher reuses the returned block.

**The return header rides that reverse edge (§3.5).** The release half of (T2) already
publishes every earlier write by the reader, including its writes to `ret[read_index]`.
`ret[read_index]` is an ordinary object and needs no atomic of its own. The return channel
is therefore a payload on a synchronization this protocol already performs and already
tests, not a second mechanism.

**Memory orders.** (P2) and (T2) are `acq_rel`, for both directions of ownership. That one
pair carries the payload forward and the return header back. Nothing else in the protocol is
atomic.

**Progress.** Publish and take are wait-free, in one exchange each. A take on a paced
engine view also makes its declaration, one short lock on `gate.m` (§3.1, §3.3).

**Failure and deadline behaviour.** The only failure is the local API refusal while array
views of the previous block are alive (§3.1). A due publish cannot be skipped, and a take
cannot fail for any reason outside the caller.

**`hardware_destructive_interference_size` depends on build flags.** It can be overridden
with `--param` and follows `-mtune`, and GCC 16 does not warn when it is used in an
ABI-visible position. **A heap-allocated engine view is deliberately not ABI-stable**: it is engine
memory, never serialized, and its layout never crosses a module boundary. The deferred
shared-memory engine view is the opposite case, and fixes every field width and offset as literals
(`design_modding.md` §4.3).

**Verification.** A functional test cannot tell a correct implementation from a broken one
on x86-64. The sanitizer test checks:

- the permutation invariant;
- the reverse happens-before edge from reader to publisher;
- riding that same edge, that a return header written before a take is read intact by the
  publisher that receives the block, and never while the reader is still writing it (§3.5).

A race between committing pages for a block or replacing it and reusing it is a required
test case.

### 3.3 Participants and the gate (normative)

An observer cannot delay a tick (§3.1). This section says what can. The gate is the only
place in the engine where the engine core waits.

**Every peripheral is one of exactly two kinds.** Which kind follows from what it does, not
from a free choice at registration:

| | **Participant** | **Observer** |
|---|---|---|
| The engine core… | waits for it, up to its declared deadline | never waits for it |
| Reads | its engine view | its engine view |
| Writes | commands | **nothing**; reading the world is not participation |
| Costs | up to its deadline of tick latency | a projection copy |
| Examples | network peers, a recorder or a mod with a paced engine view | viz and GUI, analytics, an agent mind, every logic mod without a paced engine view |

**Naming ticks makes a producer a participant; reading does not.** A paced command names the
tick it applies to (§5.1), and a tick cannot run until every source that acts in it has
finished submitting. So every producer that names ticks is paced. **Unpaced producers are
the exception**: the host in single-player, and every logic mod. Their commands name no
tick, so they have no tick to be late for (below, and §5.1). Reading gives no standing at all: an
observer may fall any distance behind, and the engine core never notices. One case is declared
rather than implied: a peripheral that submits nothing but must not miss a tick, such as a
recorder or a training-data collector. It registers as a participant explicitly, with a
paced engine view (§3.1). A logic mod follows the same rule: it is a participant only
through a paced engine view (`design_modding.md` §3).

Tier 3 engine core mods are neither kind. They are systems inside the tick, ordered by the phase
cut (§4.1), and this section does not apply to them.

**Pacing gives acknowledgement, not access.** A participant reads exactly what an observer
reads: its own engine view. Being paced guarantees that the engine core will not run ahead of what
the participant has acknowledged. It gives no view of live engine core state. **No peripheral of any
kind ever reads live engine core state.** That keeps §1's one-way boundary intact while allowing
the wait.

**State.**

```
struct Participant {
    std::atomic<u64> ready_through;   // monotone; U64_MAX = unconditionally ready
    std::atomic<u32> active;          // 0 removes it from the conjunction; CONTINUE_WITHOUT
                                      //   clears it permanently, SUSPEND reversibly
    u32              deadline_ms;     // declared at registration
    OnExpiry         on_expiry;       // FAIL | DROP | SUSPEND | CONTINUE_WITHOUT
};
struct HostControl {
    std::atomic<u64> run_until;        // exclusive tick ceiling; U64_MAX = resumed
    std::atomic<u32> stop_requested;   // sticky: 0 -> 1 only
    bool             step_in_flight;   // guarded by gate.m; never read by the sim
};
struct Gate {
    std::mutex              m;          // held for loads, stores and notifies only
    std::condition_variable sim_cv;     // the sim parks here, and nothing else does
    std::condition_variable progress_cv;  // threads waiting for a tick to run park here
    Blocker                 parked;     // why the sim is parked; NONE while it runs
    u32                     progress_waiters;
};
Participant       participants[N_PARTICIPANTS];   // fixed at the freeze
HostControl       host;
Gate              gate;
std::atomic<u64>  first_unexecuted;               // sim-written; the next tick not yet run
u64               backlog;                        // §5.2; PLAIN, sim-thread-only
```

**Every gate input changes under `gate.m`.** That covers each `ready_through` and `active`,
`run_until`, `stop_requested` and `first_unexecuted`; `parked`, `step_in_flight` and
`progress_waiters` are read and written only under it. The inputs stay atomics so that the
sim can check them without the mutex while nothing blocks.

Each cross-thread host field is its own naturally aligned atomic, on purpose. A stop, a
pause and a run grant are independent facts, so storing one must never erase another.

**Nothing waits, logs or calls out while holding `gate.m`.** Every critical section is a few
loads and stores and at most one notify, except the condition-variable waits themselves,
which release the mutex while they sleep. So taking it costs one uncontended lock, never a
wait on another party.

**The event backlog is not a host field.** `backlog` is a plain `u64` owned by the sim
thread alone. The gate reads it like any other local variable. It is not an atomic, and in
particular not a flag, for the reason §5.2 gives.

**The loop.** This is the engine's whole tick loop. The engine core's only wait on another party is
the one marked:

```
for (;;) {
    while ((blocker = blocker_for(t)) != NONE) {             # (G1) no lock: acquire loads
        if (blocker == STOP) return;
        std::unique_lock lk(gate.m);
        if (blocker_for(t) != blocker) continue;             # (G2) re-check under the mutex
        gate.parked = blocker;
        woke = gate.sim_cv.wait_until(lk, absolute_deadline(blocker, t),
                                      [&] { return blocker_for(t) != blocker; });
                                                             # THE ONE WAIT; releases gate.m
        gate.parked = NONE;
        lk.unlock();
        if (!woke) on_gate_timeout(blocker, t);
    }
    drain_commands(t);                                       # §5.1
    execute(t);                                              # §4.1 phases + commits
    publish(t);                                              # §3.2, per due engine view
    {   std::lock_guard lk(gate.m);
        first_unexecuted.store(t + 1, release);              # (G5) the tick has run
        if (gate.progress_waiters) gate.progress_cv.notify_all();
    }
    ++t;
}

blocker_for(t), in priority order:
    if host.stop_requested.load(acquire): STOP
    if backlog >= high_water: EVENT_BACKLOG                  # recomputed from the ring (§5.2)
    if host.run_until.load(acquire) <= t: HOST_PAUSE
    first active p with p.ready_through.load(acquire) < t: p
    otherwise: NONE
```

The thread that runs this loop is the **executor**: the sim thread, or the thread inside
`step(n)` when there is no sim thread. Only the executor touches engine core state while a tick
can run.

With no sim thread, `step(n)` runs this loop on the calling thread. Where the sim thread
would park on the host's own grant (`HOST_PAUSE`) or stop, the calling thread returns
instead. It checks for signals at tick boundaries, and an interrupt ends the step there
(`design_python_api.md` §4.3).

A participant becomes ready, and a control call changes a host field, the same way:

```
change(input, value):                                        # declare ready, pause, stop, ...
    std::lock_guard lk(gate.m);
    input.store(value, release);                             # (G3) LINEARIZATION POINT
    if (gate.parked != NONE) gate.sim_cv.notify_one();       # (G4) wake only a parked sim
```

**The wake-up rule (normative).** Every predicate a parked thread waits on changes only while
holding that park's mutex, and the change is followed by a notify. Every park in the engine
follows it: the gate, the progress waits below, the admission wait (§5.1) and the event
backlog (§5.2). A change made outside the mutex can land between a waiter's check and its
wait, and the waiter then sleeps through it. No ordering argument repairs that, so none is
attempted.

**Why the rule makes the wake-up correct.** The sim decides to sleep only after re-checking
under `gate.m` (G2), and `wait_until` releases the mutex in the same step as it sleeps. A
writer stores under the same mutex (G3). So the writer either finishes before the re-check,
which then sees the new value and does not sleep, or starts after the sim is parked, sees
`parked` set and notifies (G4). There is no third order. The condition variable's
notifications are never the condition: the sim re-evaluates `blocker_for` every time it
wakes, so a spurious wake-up costs one loop iteration and a notify to a sim that has already
left costs nothing.

**Why this costs almost nothing.** While nothing blocks, (G1) finds no blocker and the sim
takes no lock before the tick. It takes one uncontended lock per tick at (G5). Each
participant takes one per declaration, and each control call one per change. These are tens
of nanoseconds on paths that run once per tick. Waking only a parked sim (G4) keeps
notifications off the healthy path. `wait_until` also gives the deadline that atomics lack:
C++26 `<atomic>` has no timed wait.

`ready_through` only ever increases, and so does `first_unexecuted`. A waiter that wakes late
still finds a true condition true. Neither is ever a flag that is true only briefly.

**Waiting for a tick to run.** A thread that waits for the sim to reach a tick parks on
`gate.progress_cv` under `gate.m`. The stepping owner thread waits there
(`design_python_api.md` §4.3), and so does a paced producer reading an outcome (§5.1):

```
wait_for_tick(target, until):                                # a stepping owner, an outcome read
    std::unique_lock lk(gate.m);
    ++gate.progress_waiters;
    ok = gate.progress_cv.wait_until(lk, until,
             [&] { return first_unexecuted.load(relaxed) >= target || other_wake_reason(); });
    --gate.progress_waiters;
```

The sim stores `first_unexecuted` under the mutex after each tick's publish and notifies if
anyone waits (G5), so this park follows the wake-up rule like the gate does. `until` lets a
caller wait in slices: the owner thread checks for signals between them.

**Deadlines are absolute and share one anchor.** On the first blocker for tick `t` that is
not a pause, the sim records one `wait_started[t]`. Every participant's deadline for that
tick is `wait_started[t] + participant.deadline`, even if `blocker_for` reaches that
participant later. A spurious wake-up never recomputes either value from `now`. So
participants use up their budgets at the same time, and the total first-attempt latency is
bounded by the largest deadline, not the sum. An explicitly documented retry gets one new
anchored budget.

**A pause stops every clock that can abort something.** This is a correctness rule. A paused
session is not advancing, so nobody in it can be late. A deadline that kept running through
a pause would turn "the operator stopped the game" into "the engine dropped a peer". That is
the same defect as dropping a peer on a local timer (`DROP`, below). So when `HOST_PAUSE`
becomes the blocker, the sim **discards `wait_started[t]`**. The first blocker after the
pause that is not a pause anchors a fresh one, and every participant gets its full declared
budget again. The rule covers every clock in the engine:

| Clock | During a pause |
|---|---|
| participant deadlines at the gate | re-anchored on resume; a pause is never charged to a peer |
| a producer's admission or outcome wait (§5.1) | none to stop: these waits have no timer. The waiter stays parked and continues when the session does |
| a mod's inbox-drain and `on_unload` budgets (`design_modding.md` §6) | suspended. Shutdown is not a pause, so this matters only if a pause overlaps shutdown |
| the event backlog (§5.2) | it is itself a pause, so there is no second clock to stop |

**A pause leaves waiters parked.** An admission or outcome wait is bounded by structure, not
by a timer (§5.1), so a pause cannot make it late, and waking it would only tell the caller
to wait again. So a pause wakes nobody. Inside a mod, a paused session looks like one long
tick, which is what it is. Only revocation wakes a waiter early, with `revoked` (§5.1), and
`close()` revokes every endpoint it has not already revoked (`design_python_api.md` §2).

`HOST_PAUSE` is intentionally indefinite. Each wait uses a bounded diagnostic slice whose
expiry means `remain parked`, never `FAIL`. `EVENT_BACKLOG` works the same way,
for the same reason (§5.2): an application that stops draining events pauses the simulation
and is reported, rather than timed out into a dead session.

**Progress.** The sim blocks, bounded by the smallest declared deadline among participants
that are not ready yet. Each participant declares readiness with one store under `gate.m`:
a short lock, never a wait on the sim. Participants wait at the same time, so the worst latency the gate adds to a tick is the
largest deadline in the blocking set, not the sum. That is what makes several participants
affordable.

**The host is not a participant.** The gate never waits for it as a command producer:
its commands name no tick (§5.1), so there is no tick it must finish submitting for. What
the host controls is whether ticks run at all. Its predicate is a combination of
independent fields rather than one overloaded word:

| Host state | Representation |
|---|---|
| running | `run_until = U64_MAX`, `stop_requested = 0`, `backlog < high_water` |
| `pause()` | under `gate.m`, set `run_until = first_unexecuted` |
| `step(n)` | under `gate.m`, reserve `step_in_flight`, then set `run_until = first_unexecuted + n` (checked for overflow) |
| event backlog at its high-water mark (§5.2) | **not a host field at all**: `backlog` is a plain sim-local value, so a resume cannot race it and a drain cannot erase it |
| `request_stop()` | under `gate.m`, `stop_requested.store(1, release)`; sticky and highest priority |

**A host command cannot miss its tick, because it names none.** The host endpoint is
unpaced (§5.1). The engine applies each host command at the first tick whose drain finds
it, and the command's outcome reports that tick. So the frame loop never holds the sim
back: a slow frame, a breakpoint or a dragged window does not stop the simulation. The
record stays exact, because commands are recorded as they are consumed, with the tick that
ran them (§2.3). Which tick a live host command lands in depends on timing, which session
reproducibility already allows (§2).

This holds in single-player only. In a networked session every machine must run a player's
command at the same tick. So the local player submits through its own peer endpoint, with
the input delay, and that endpoint is paced like any other peer's (`design_multiplayer.md`
§3.2). How that endpoint is produced is open
([Q55](open_question.md#q55-the-local-players-source-id)).

**The owner thread never waits on the engine; it polls.** The owner thread runs the frame
loop, which drains events (§5.2) and presents frames. A call that parked it on the engine
could wait for a sim that is itself waiting for a drain, or freeze the window while the sim
is paused. So every call the frame loop makes returns at once:

| Call | When it cannot complete now, it returns |
|---|---|
| host `submit` | `queue_full`: the host endpoint already holds `capacity` commands not yet drained (§5.1) |
| `outcome(h)` | `pending`: the tick that drains the command has not run yet |
| `drain_events()` | an empty batch; it never blocks |

Two kinds of call wait, and they wait only on the sim or on a deadline, never on
something only the owner thread can do. `step(n)` waits for the sim to run its ticks. If
the next tick is blocked by a condition only the owner can clear, such as the event backlog
(§5.2), it returns or raises instead of waiting. A paced engine view that the stepping
thread itself reads is such a condition too, but the engine does not see it, so that reader
steps no further than its next publish (§3.1). The shutdown calls, `stop_sim_async()` and
`close()`, wait under the shutdown deadline (`design_python_api.md` §2). `snapshot()` does
not wait: the owner thread makes the copy itself, while no tick can run (§3.1). Peers keep
their waits, and a logic mod may wait for an outcome (§5.1). They run on their own threads,
and the owner thread never waits for them.

Pause, single-step, backpressure and shutdown still form one gate predicate, but no two
share storage, so concurrent controls cannot lose a stop or unblock a backed-up ring.
`pause`, `resume`, and the setup and teardown of `step` serialize on `gate.m`, the mutex
every gate input already changes under. So `resume()`'s "no step in flight" check and its
store form one transition, not a check-then-act race. The backlog itself has one writer,
the sim; what the owner changes is the ring's `read` index (§5.2).

`run_until` is an exclusive ceiling, so pausing before tick 0 can be represented without
unsigned underflow: tick `t` may run exactly when `t < run_until`.

`step(n)` records `start = first_unexecuted`, sets `run_until = start + n`, and returns when
`first_unexecuted >= start + n`, waiting on `gate.progress_cv` (above). The host's own run
grant then blocks the gate, so a sim thread is provably parked when the call returns. That
makes `checksum()` on the next line a well-defined read of quiet state rather than a race
(`design_python_api.md` §4.3, §6). The wait has no deadline, and an interrupt ends it
(`design_python_api.md` §4.3).

**Stopping ends ticking for the session.** `stop_requested` is never cleared, so once the
sim has met it at the gate, no tick runs again. A stopped session can still be read
(`snapshot()`, `checksum()`) and closed (`design_python_api.md` §2).

**Registration** happens before the freeze, which closes the set (§2.4 step 3a). It declares
the deadline and the expiry policy. There is **no engine-wide default** for participants in
general. Registration is where the cost is taken on, so it is where the cost is declared,
just as a mod manifest declares its endpoint capacity. Mods are the one case with a
fallback, because a manifest may leave its deadline out (below). The count is capped (`design_limits.md` §2.1). The cap is generous
because a participant costs an array entry scanned once per tick, not an allocation.

A mod's deadline comes from its manifest's `deadline_ms`, or `HostPolicy`'s default when the
manifest gives none, and `HostPolicy` caps it (`design_modding.md` §3,
`design_python_api.md` §3). A mod's expiry policy is derived, not declared: its paced
engine view is continued without (`CONTINUE_WITHOUT`). A mod's commands name no tick, so the
gate never waited for them, and dropping the engine view from the conjunction cannot change
them. Those are the only two answers that cannot change what the session computes behind its
back (below).

**On expiry.** The participant set never shrinks, since the freeze fixes it. What changes is
whether a participant can still hold the gate. The `DROP` row is a correctness constraint,
not a preference:

| `on_expiry` | Behaviour | Use |
|---|---|---|
| `FAIL` | log `critical` naming the participant and tick, retry once, then the engine enters the terminal `failed` state | a participant whose lateness is a defect, not a load condition |
| `CONTINUE_WITHOUT` | `active.store(0, release)`, so later readiness stores cannot re-enter the conjunction; emit a reliable-class event; keep ticking | a paced recorder or mod whose absence cannot change what the engine core computes |
| `DROP` | **escalate to the server tier and accept its answer**; never remove the participant locally | a network peer |
| `SUSPEND` | stop feeding it, keep its endpoint, and take it out of the conjunction until it drains; reversible | a mod that is not keeping up (`design_modding.md` §4.2) |

The obvious implementation of `DROP` is wrong in two ways:

- **Who decides.** A peer's commands are part of the command set for tick `t`. An instance
  that drops a peer on a local timer runs a different set from every other instance and
  desyncs. So the local gate never removes a peer on its own. It reports the expiry upward,
  and the **server tier** decides: the few server nodes that agree among themselves over
  links treated as reliable, and the only nodes that may conclude someone has dropped. A
  client node receives that conclusion and never forms one. Until the server tier answers,
  the local response is the same as for `HOST_PAUSE`: pause and report (§5.2's table).
- **When it takes effect.** Agreeing that a peer dropped is not enough. Removing it changes
  the command set, so it must take effect at an **agreed tick**, carried in the turn stream
  like any other change. Two instances that apply the same drop at different ticks diverge,
  just as they would with local timeouts. The server tier settles who decides; the agreed
  tick prevents the desync. The stall policy and escalation protocol are open
  ([Q48](open_question.md#q48-stall-policy),
  [Q49](open_question.md#q49-peer-deadline-and-drop-escalation)).

**Leaving the gate.** A participant whose producer has stopped leaves the conjunction at
once, without waiting out its deadline. When a mod host stops, by quarantine, a failed spawn
or `mods.stop()` (`design_modding.md` §4.2, §6), the engine clears the `active` flag of the
mod's participant, its paced engine view, under `gate.m` and notifies a parked sim. It
reports the departure as a reliable `participant.left` event. A retry re-enters it at the
current tick: under `gate.m`, its `ready_through` becomes `first_unexecuted - 1` and
`active` becomes 1. Suspension (`SUSPEND`) and resumption use the same two stores.

`CONTINUE_WITHOUT` is allowed only for a participant whose absence provably cannot change
the command stream. That excludes a peer, whose commands name ticks the gate waits for. A
mod's paced engine view qualifies even when the mod submits, because the mod's commands
name no tick and never depended on the gate. `SUSPEND` is what the mod bus applies to a mod
whose inbox overflows (`design_modding.md` §4.2). It takes a paced engine view out of the
conjunction too, is reported, and is reversible; it never ends the session.

**In single-player with no paced engine view, the conjunction is empty.** While the session runs,
the host's run grant is `U64_MAX`, `stop_requested` is clear and the backlog is below its
mark. So the gate costs two loads and one plain comparison per tick, and it blocks only on a
pause, the end of a step or the event backlog. The gate built for multiplayer costs only
this at its default setting (`design_multiplayer.md` §6).

**Verification.** A lost wake-up is a hang, not a data race, so ThreadSanitizer does not
report it. The wake-up rule makes each park correct by construction, and stress tests check
that the construction was followed. Each test drives one park against its writers many times
under a watchdog timeout, and a timeout fails the test. The tests cover the gate against
declarations and control calls, the progress waits against the sim, the admission wait
against the drain (§5.1), and the backlog wake (§5.2). They run under CTest. Data handed
between threads is still checked by the sanitizer tests of §3.2 and §5.1.

### 3.4 Row predicate (normative)

**Each engine view filters on two axes.** Columns are the projection spec (§3.1). Rows are this
section. Without row filtering, an engine view over 10⁷ live rows copies all of them to a reader
that wants 10⁴. No column choice fixes that: a 32-byte projection of 10⁷ rows is still 320
MB per publish (`design_limits.md` §5).

**A row predicate cannot cause a desync**, and the rest of this section relies on that. An
engine view is an output; engine core state cannot be rebuilt from a snapshot (§5). So two peers may
filter to completely different row sets and stay in lockstep. The predicate needs no
determinism, takes no part in the checksum, and never enters the command stream.

**The kind is declared; the parameters are not.** An engine view registers one kind from a closed
set:

| Kind | Parameters | Use |
|---|---|---|
| `ALL` | none | the default, and what every engine view has until measurement says otherwise |
| `AABB` | min, max | |
| `SPHERE` | centre, radius | the agent case: "what is near me" |
| `FRUSTUM` | six planes | the render case |
| `TAG` | mask | over a declared bitset column |

**The predicate is not a callback.** An arbitrary `bool(row)` would put unbounded
third-party code on the sim thread and destroy the property this document depends on: cost
that is knowable at construction. It could not be vectorized, bounded, or blamed when slow.
The kind is fixed at the freeze so the cost model is closed. Only its parameters change per
publish, through the return header (§3.5).

**The engine chooses how to evaluate a kind.** A linear scan over the predicate's input
columns always works, and it is the baseline charged in §3.1. If the engine core already keeps a
spatial structure for its own systems, `SPHERE` and `FRUSTUM` may be answered from it, and
the scan cost disappears. That is an implementation choice behind the declaration. A reader
never declares or depends on it.

**The algorithm is already specified elsewhere.** Evaluate the predicate to a mask, AND it
with the live bitmap, exclusive-scan the mask into destination offsets, and gather each
column. That is exactly the publish scan and gather (`design_data_container.md` §5.1), which
every engine view runs anyway; `ALL` is the live bitmap alone. The scan keeps ascending slot order
without extra care. The gather may run in parallel on the worker pool, because every
destination comes from the prefix sum and the world does not change between the terminal
commit and the next tick. Where the scan is charged is open
([Q35](open_question.md#q35-where-is-the-predicate-scan-charged)).

**Coarse in the engine core, exact in the reader.** The engine core's predicate is conservative: a
region, not a final visibility answer. The reader refines it against its own fresh camera, on the
rows that passed. This split has three benefits:

- the engine core never needs the exact camera, so the one-tick staleness of the parameters (§3.5)
  needs no safety margin: the region is already loose;
- refinement is always safe: filtering too little draws too much, filtering too much drops
  something the reader is responsible for, and neither can reach engine core state;
- the parameters stay small, so the return header can be a fixed struct rather than a
  variable-length message.

**Every engine view carries the id column unless it declines it.** Rows 3, 17 and 902 mean
nothing to a reader without the id column. So every projection includes `id`, at 8 bytes per
matched row, whatever its predicate. A reader that never needs to know which entity a row is
declares `identity=False` at registration, and the freeze closes that declaration with the
rest (§2.4 step 3a). The render engine view is the usual case. The GPU may not key on a
snapshot row across frames anyway (`design_data_container.md` §5), so a renderer that only
draws needs no id and does not pay for one. Looking an entity up by id in such an engine
view's snapshot raises (`design_python_api.md` §7.2). An on-demand snapshot always carries
`id` (§3.1).

**Filtering is turned on by measurement.** `ALL` is the default, and it is right at 10⁵ live
rows, where a full projection is a few megabytes and a predicate would add complexity for no
gain. Row filtering is a scaling feature, turned on when `publish bytes/second` calls for
it.

### 3.5 The return header (normative)

**The take exchange already works in both directions.** The last part of §3.2's theorem says
so. The release half of (T2) pairs with the acquire half of the next (P2). So every write
the reader makes happens before the publisher reuses the returned block. A reader that
writes before it takes has already published those writes, through an edge that exists
whether or not anything uses it.

So the return channel costs **no new atomic, no new ordering argument, and no new sanitizer
mechanism**. It is a payload on a synchronization that already happens.

**State.** One fixed struct per block, in an array parallel to the blocks:

```
struct ReturnHeader {                // consumer-written, publisher-read
    u32              seq;            // monotone; publisher keeps the newest it has seen
    u64              last_consumed_tick;
    u32              cadence_hint;   // 0 = no preference
    PredicateParams  params;         // §3.4; fixed size, kind-dependent interpretation
};

ReturnHeader ret[3];                 // parallel to block[3]; never reallocated
```

The headers sit in a parallel array rather than inside the blocks, for two reasons. A block
commits pages as its matched rows grow (§3.1), and a header should not depend on which pages
are committed. And a header in a block's first cache line would false-share with the
publisher's fill. `ret` has a fixed size and never grows.

**Protocol.**

```
consumer:  fill ret[read_index]            # plain; read_index is the block it holds
           take()                          # (T2) release-exchange publishes the fill

publisher: on receiving block b at (P2), read ret[b]      # acquire half makes it visible
           if (ret[b].seq > seen) { seen = ret[b].seq; adopt params, hint, tick }
           fill *block[b]
```

The publisher keeps its own copy and adopts only a newer `seq`. Different blocks carry
headers of different ages. A reader that stops taking simply goes stale; its header is never
torn. Because `seq` only increases, "the newest I have seen" is a comparison rather than a
protocol. Parameters are therefore at most one publish cycle old, which the coarse/exact
split of §3.4 absorbs.

**What may flow back, and what may not.**

| | |
|---|---|
| **May** | predicate parameters (§3.4), `last_consumed_tick` (the lag metric of §3.1), a cadence hint |
| **May not** | anything the simulation reads |

**The return channel may shape the engine view; it must never reach the world.** A cadence hint
changes how often this engine view is published, and nothing else. A predicate parameter changes
which rows this engine view contains, and nothing else. Anything the engine core acts on goes
through the command ring (§5.1), which is the ordered, deterministic, replayed and checksummed input
path. A second input that is unordered, undeclared and invisible to replay would be a desync
source. This channel is convenient, which is exactly why the rule is stated here.

**The cadence hint is advisory.** The publisher may ignore it, and always ignores it on a
paced engine view, whose cadence sets its declarations (§3.1). A reader that must receive
every tick registers a paced engine view and becomes a participant (§3.3), paying for that
guarantee at the gate. The hint lets a reader shed load without blocking the engine core; pacing
lets it refuse to miss a tick. They meet different needs, and neither replaces the other.

**Trust.** The return header keeps `PRIVATE`'s trust boundary; it does not widen it. The
publisher already uses the block index its reader writes (§3.2), so a reader able to corrupt
the header could already do worse. For the same reason the return header does not extend to
a sandboxed reader. A `SHARED` engine view exposes no reader-written bytes that the publisher
dereferences (Appendix A), and giving it a return header would be a new decision, not this
one extended.

## 4. Parallelism inside the deterministic engine core

The engine core can be highly parallel. The only constraint is that **the merged result must not
depend on scheduling**.

**Units.** There are exactly two, and neither is spatial. Neither is called a "partition":
in this codebase that word means only a C++20 module partition (`design_patterns.md` §1).

| Unit | What it is | Unit of |
|---|---|---|
| **chunk** | a fixed range of slots within a column. Its size is defined once, as `LIBSIM_ESTAB__CHUNK_ELEMENTS`, in `design_data_container.md` §4 | SIMD iteration, false-sharing isolation, parallel-for work items |
| **task range** | the contiguous run of whole chunks given to one parallel invocation | write scoping: the `begin` and `end` of a mod's write span (`design_data_container.md` §7.3) |

Task ranges never overlap, by construction: the parallel-for hands out non-overlapping chunk
runs, so no system ever has to assert it.

A row is an entity's slot. It stays the same for the entity's whole life, but only ids cross
a boundary (`design_data_container.md` §2.2). Everything below is written in terms of rows,
because nothing inside a phase needs an id.

### 4.1 Phase-structured update (read → compute → deterministic commit)

Each tick runs its systems in phases. The phase invariant below limits how systems in one
phase may share columns, so that the phase can run in parallel. Between phases, and once at
the end of the tick, commits apply buffered changes in a fixed order.

#### Per-column access declarations

A system declares an access kind for each column it uses, not one model for itself. A system
that reads five columns and writes two makes seven declarations. The set of kinds is closed:

| Kind | May read | May write |
|---|---|---|
| `read_full` | any row | nothing |
| `read_local` | its own task range | nothing |
| `write_local` | its own task range | its own task range, in place |
| `write_staged` | any row | a per-task buffer, merged in fixed task order at commit |
| `write_dbl` | any row of the **front** buffer | its own task range of the **back** buffer; flipped at commit |

Reflection turns these declarations into read-only or writable spans of the right extent
(`design_data_container.md` §4). So the span a system receives is its permission.

#### The phase invariant

> Within one phase, every column satisfies both clauses:
>
> 1. **At most one writer**: one system declaring any of the three `write_*` kinds.
> 2. If that writer declared **`write_local`**, then **no system in the phase may read the
>    column outside its own task range**: no `read_full`, and no `write_staged` either,
>    since `write_staged` may read any row.

This makes "no data races" a structural property. Clause 2 is the half that is easy to
forget. Scoping writes to separate ranges is not enough: reading, across ranges, a column
another task is writing in place is already a data race and already depends on scheduling.
So for a column written in place, the read span is as limited as the write span. A column
that anyone reads across ranges must use `write_staged` or `write_dbl` instead.

Clause 2 deliberately exempts `write_dbl`, and that is the point of `write_dbl`. The front
buffer does not change during the phase, so any number of systems may read any of its rows
while the single writer fills its own rows of the back buffer. Cross-range reads and
in-place writes are kept apart in memory rather than in time. Clause 1 still applies: two
writers to one back buffer would race.

Two rules are stricter than they need to be, on purpose:

- Two *different* systems writing separate ranges of one column would be safe, but clause 1
  forbids it. The normal case, one system spread over N task ranges, is a single writer and
  is allowed.
- `read_local` on a column nobody writes is allowed but adds no safety. It exists so a
  system can choose the tighter span and have the scheduler enforce it.

How systems inside one phase are scheduled is not yet specified
([Q37](open_question.md#q37-how-do-systems-in-one-phase-run)).

#### Deriving the phases

Systems run in a fixed total order (§4.5), so phases are cuts in that sequence, never a
reordering:

1. Walk the system list in declared order, adding systems to the current phase.
2. Before adding system S, check the invariant for every column S declares.
3. If it fails, close the phase (barrier, merge `write_staged` buffers in fixed task order,
   flip `write_dbl` columns) and start a new phase with S.
4. After the last phase, run the **terminal commit**: all structural changes, once.

The cut is computed once, at the freeze (§2.4), in O(systems × columns). It cannot run
earlier, because the system list is not closed until every engine core mod has registered. It must
not run later, because the first tick needs it. The cut is derived state, not part of
session identity: the same mod set gives the same list in the same order, and so the same
cuts. A replay therefore reproduces the schedule without the header carrying it. Because the
system order is fixed, the cut points are fixed, so **the phase structure cannot change with
the thread count**. §7 step 4 sets out to prove exactly this. A system whose declarations
conflict with both neighbours ends up alone in its own phase; that is all "sequential" means
here, not a separate execution mode.

#### Commits: two different kinds

"Commit" names two different points. Mixing them up would misprice the tick, so they have
separate names:

| | **Boundary commit** | **Terminal commit** |
|---|---|---|
| When | at every phase close (step 3) | once, after the last phase (step 4) |
| Does | merges `write_staged` buffers, flips `write_dbl` columns | applies **all** structural changes: erases with their link fix-up, then creates and relationship rewiring. Moves no row |
| Changes the live set? | never | yes, and only here |

Both apply their staged input in a fixed order, by task index or row, never by the
order threads finish, to produce tick N+1. The one exception, and why, is at the end of this
subsection. Staged buffers with a fixed-order commit are required for anything that might
conflict or change structure. That covers shared claims, reductions that are not
order-independent, create and delete, relationship rewiring, and any write whose destination
is found at run time. They are not required for ordinary `write_local` column writes. Tier 3
mods sign the same contract (`design_modding.md` §5.2).

**Structural changes happen only in the terminal commit.** This has four consequences:

- **The live set is stable for the whole tick**, not just within a phase. A row holds the
  same entity from the first phase to the last, so no tier has to count phases
  (`design_data_container.md` §2.2). Rows never move at all; only the terminal commit changes
  which slots are live.
- **The terminal commit costs O(changes), not O(rows).** Erases clear bits and zero rows,
  creates fill the lowest free slots, and nothing moves (`design_data_container.md` §2.2).
  Its cost is O(erases + creates + links into erased entities), one small line in the tick
  budget (§3.1).
- **Creation is deferred.** An entity a system creates in phase 1 cannot be addressed until
  the next tick, because it has no row until the terminal commit assigns one. flecs and
  Unity DOTS defer structural changes the same way, and it is the price of the first two
  points. A system that must act on a new entity in the same tick has to run after whatever
  creates it, and read the staged create buffer instead of the columns.
- **Every way a create can fail is decided here.** Every object type has a cap, and its
  memory is reserved at the cap at the freeze (`design_data_container.md` §2.2). The commit
  commits the pages it needs before it changes anything. **Commit failure ends the session;
  it is never a rejection.** A rejection caused by host memory would make two machines
  replaying the same stream accept different creates.

  The commit accepts creates only while free slots remain. The free count is taken after
  the erases, since erases staged this tick free their slots in the same commit. Staged
  create buffers then merge in fixed task order: the first that fit are accepted, and the
  rest are rejected and reported. Task order makes the accepted set a function of state, not
  of scheduling. Without it, a full pool would turn the parallel phase into a desync source,
  which is the one thing §4 exists to prevent. Accepted creates take the lowest free slots,
  in the same order. What kind of cap an object type has, and what "stop" means for a
  ceiling, are open ([Q31](open_question.md#q31-what-does-hitting-a-declared-cap-do)).

A boundary commit is cheap by construction: it touches only the columns that systems in the
closing phase actually staged. Neither commit is O(rows).

**Both commits are sequential.** The staged merge runs in fixed task order, and that order
is the determinism guarantee. Running it on the worker pool would bring back exactly the
dependence on completion order that staging exists to remove. The terminal commit's erase
and create steps are sequential too. They touch only the entities that changed, so no
O(rows) part is left to spread over the worker pool.

flecs staging works this way (per-thread command queues merged sequentially at sync points),
and so does Unity DOTS `EntityCommandBuffer` playback.

#### Worked shapes

The right kind is obvious only for the first case:

- **Per-row update** (`wealth[r] += income[r]`): `write_local` on `wealth`, `read_local` on
  `income`. Fully parallel, and the common case.
- **Reduction to one value** (total wealth, a treasury): `read_full` on the source. The
  destination is not a column, so it is `write_staged`: per-task accumulators merged in
  fixed task order. Wrapping integer arithmetic makes the result order-independent anyway
  (§4.2).
- **Many-to-one across objects** (pops paying tax into a province row): several tasks target
  the same destination row, so `write_local` is not allowed and `write_staged` is required.
  This is common in an establishment sim, and it is also the case the scatter rule in
  `design_data_container.md` §3 constrains.
- **Update that touches neighbours** (diffusion, smoothing): it reads rows outside its own
  range of a column it also writes, which `write_local` forbids. `write_dbl` is the intended
  answer: read neighbours from the front buffer, write your own range of the back buffer,
  flip at commit. `write_staged` also works and needs no extra column, but pays for a merge;
  prefer it when writes are sparse. There is no spatial-colouring alternative (see the note
  below).

#### Enabling guarantee

**No structural change happens during the tick's phases.** The live set, the extent and
every generation are fixed from the first phase to the last, because creates and erases
happen only in the terminal commit (`design_data_container.md` §2.2). Without this, the
read-span rule above could not be proven, because a row could change occupant in the middle
of a tick.

Rows never move, and neither do addresses. Every column is reserved at its type's cap at the
freeze, and the terminal commit only commits pages inside that reservation
(`design_data_container.md` §2.2). A pointer handed to a system is still scoped to one
`tick_fn` call (`design_data_container.md` §7.3), because the permission it carries changes
between phases, not because the memory moves.

**No spatial colouring.** Checkerboard colouring over a spatial grid, as Factorio does, is
deliberately not part of this design. Colouring only means something over a spatial unit,
and a spatial region's rows are an arbitrary scattered subset of slots. So colouring
the chunks above would separate nothing and guarantee nothing. Making it work would need a
cell column derived from position, a deterministic index from cell to rows, declared halos,
and a write span that checks membership: a whole subsystem, not a rule. `write_staged` is
already correct for every system that touches neighbours, at some throughput cost and no
correctness cost, so it is the permanent answer here.

### 4.2 Order-independent reductions

With wrapping integer arithmetic, sums, minimums, maximums and XORs are associative and
commutative, so the order of a parallel reduction does not matter. Float engines do not have
this luxury. Use it for aggregate statistics and resource totals (wrapping through unsigned
representations, §2.1).

**The verification checksum must not be a plain order-independent reduction.** This is an
easy mistake. A reduction that ignores order cannot see a permutation, and "two entities'
values were swapped", the typical failure of an iteration-order bug (§2.2), is a
permutation. The one kind of bug the checksum exists to catch would be the one it cannot
see. So the checksum must be either a hash streamed in a canonical order, or an
order-independent reduction *with each row's id mixed in*. Canonicalizing the input (§2.3)
gives neither by itself. Slot order is canonical (`design_data_container.md` §2.2), so a
hash streamed in slot order would also be correct.

**The checksum is the id-mixed reduction** (`design_limits.md` §6). Two properties decide
it. First, it splits per chunk: each chunk's partial result combines with the others in any
order. So the full checksum can run in parallel, and the rolling checksum (§2.3) can hash one
slice of chunks per tick. A streamed hash can do neither. Second, the id contains the slot
(`design_data_container.md` §2.2), so mixing in the id also covers which slot each entity
sits in. The function is a hand-written 64-bit mix rather than an external hash, because
bit-exactness across compilers matters most, and it vectorizes over SoA columns. Its bit-level specification belongs to §7
step 2 and is open ([Q27](open_question.md#q27-the-checksums-exact-algorithm-and-inputs)).

### 4.3 What to avoid

- **Concurrent containers** (concurrent hash maps and the like): insertion order depends on
  scheduling. The Unity DOTS lockstep project had to remove `NativeMultiHashMap.Concurrent`
  for exactly this reason.
- **Atomics that pick winners** ("the first thread to claim X wins"): the winner depends on
  timing. If contention needs resolving, collect all claims, then resolve them in a
  deterministic pass (sort by row).
- **False sharing**: Factorio's first attempt at parallel update was slower than serial,
  because unrelated tasks' data shared cache lines. The fix is physical separation of data:
  per-chunk allocators, 64-byte-aligned SoA blocks, and each task range's working set on its
  own pages.
- **Work stealing that changes results**: stealing is fine for scheduling, as long as the
  work items and their commit order are fixed. It must never change which task range
  processes what.
- **Scatter without proven unique indices**: if a SIMD scatter's index vector repeats a row,
  which lane wins is unspecified, so the result can change with lane width even on one
  thread. Separate task ranges do not help, because they say nothing about duplicates within
  one vector. The rule is in `design_data_container.md` §3.

### 4.4 SIMD

Integer SIMD is fully deterministic: the same bits on SSE, AVX and NEON. So the engine core can use
SIMD heavily without risk, provided that:

- **Hot component data is SoA** (see §5): contiguous `int32` and `int64` columns, 64-byte
  aligned. The chunk size is defined once, as a build constant, in
  `design_data_container.md` §4.
- **Algorithms do not depend on lane width.** Per-element operations and wrapping reductions
  already don't. Anything sequential (prefix scans with saturation, one RNG per lane) needs
  a fixed logical order whatever the physical width. A scatter with duplicate indices breaks
  this rule, because which lane wins depends on width and code generation. So it is covered
  by the same rule, not a separate one (`design_data_container.md` §3).
- **Multiplication widths are handled carefully.** Qm.n multiplication needs widen, shift,
  narrow. On SIMD that means `mul_even` / `mul_hi` style patterns, kept inside the `fixed`
  type's SIMD kernels.

The SIMD layer is `std::simd` (C++26 `<simd>`, from `std::experimental::simd`). It fits this
GCC 16, gnu26 modules codebase, and its libstdc++ coverage was checked in the build
container (`design_data_container.md` §3). Whether it is enough for fixed-point
multiply-high and gather/scatter is open ([Q30](open_question.md#q30-is-stdsimd-enough)).
Other libraries:

- **[google/highway](https://github.com/google/highway)**, the fallback: portable,
  length-agnostic, with runtime dispatch (SSE4 to AVX-512, NEON/SVE, RISC-V). Runtime
  dispatch is safe here only because the engine core is integer-only: every instruction-set path
  gives identical bits. Widely used in production.
- **xsimd**: header-only and simpler, but built around static dispatch.
- GCC autovectorization of clean SoA loops over `int` columns is also good. Start with
  scalar SoA code, and add explicit SIMD kernels where profiles call for them.

### 4.5 ECS or not (flecs vs custom SoA)

flecs is already a dependency, and on paper its scheduler suits determinism: systems ordered
by entity id, table slices pinned to threads, staged commands merged sequentially. But for
the hot deterministic path it has costs. Table iteration order depends on the order
archetypes were created, its internals must be audited for behaviour that depends on
addresses, and we do not control memory layout or alignment for SIMD.

The design is a hybrid:

- **Engine core world data lives in custom SoA pools** (M3, the world container). They hold
  columns of fixed-point and integer components, indexed by slot, and generational ids whose
  slot is the row (`design_data_container.md` §2.2). Fixed chunking serves task ranges and
  SIMD alignment. Columns indexed by slot iterate faster and vectorize better than sparse
  sets. Sparse sets win under heavy add/remove churn, and a pool absorbs churn without
  moving rows.
- **flecs is optional, for the cold path**: entity lifecycle bookkeeping, composing
  peripheral displays, editor and debug queries. These are places where determinism does not
  matter. Whether to keep flecs at all is open
  ([Q56](open_question.md#q56-keep-or-drop-flecs)).
- The engine core's system scheduler can then be a simple explicit list: a fixed system order, each
  system declaring an access kind per column, and phases derived once, statically, by the
  cut in §4.1. Explicit ordering worked better than attribute-driven ordering in the DOTS
  lockstep experience.

## 5. Peripheral systems (float domain)

Peripherals are everything outside the engine core. Every peripheral here is an observer unless it
registers as a participant (§3.3). None reads live engine core state, and none can delay a tick by
lagging.

- **Renderer and viz**: reads a `PRIVATE` engine view over the `[[=viz]]` columns at cadence 1 (no
  interpolation, §3), converts fixed-point to float once, and is free to use floats, compute
  shaders and Vulkan. Nothing flows back.
  - **Threading**: preparation and GPU compute may run on workers, but presentation is a
    backend contract, not an architectural freedom. Under SDL3, the window, swapchain
    acquisition and present belong to the owner thread (`design_python_api.md` §4.3). A raw
    Vulkan backend could present from any thread.
  - **Memory**: uploads and dispatches read only the renderer's own staging and destination
    buffers, never engine view memory (the CPU/GPU lifetime boundary in §3.1).
  - **Backend scope**: Vulkan only (SPIR-V from glslang), covering Linux and Windows. D3D12
    and Metal need DXIL and MSL and are future work, so macOS has no GPU path
    (`design_data_container.md` §5).
  - **Losing the GPU device ends the process.** It is logged as `critical`, the
    out-of-process cleanup runs, and the process exits with a nonzero status
    (`design_python_api.md` §2). There is no recover-and-re-upload path. Engine core state is
    untouched and the session can still be replayed from its record, because the GPU is in
    the peripheral domain.
- **GUI**: an engine view like any other, usually a few columns at cadence 1, plus the host's
  command endpoint. It is not a special case of anything. Under lockstep, a `pause()` from
  the GUI is agreed across the session rather than applied locally, because it changes the
  run grant that feeds the gate check every peer uses (`design_multiplayer.md` §3.3).
- **Data viz and analytics**: an engine view at a low cadence. Five columns every thirty ticks costs
  what its declaration says.
- **AI**: reads an engine view (stale data is fine and realistic), thinks in floats or on the GPU,
  and emits commands. Commands are recorded in the input stream, so AI non-determinism does
  not affect the simulation. If a replay from the seed alone is ever needed, the AI must
  either be deterministic (integer inference) or its commands must count as external input;
  §6 describes the hybrid split this design uses. A planner whose reasoning spans several
  ticks must carry entity ids, not snapshot rows, and re-resolve them against a fresh
  snapshot before submitting. A snapshot row means something only inside the snapshot that
  produced it (`design_data_container.md` §5.1).
- **GPU compute for the engine core?** Peripheral-only for now. Float GPU work is non-deterministic
  in practice: atomic commit order, per-driver shader compilation and reduction scheduling
  all vary (see NVIDIA's CCCL determinism levels). Integer-only compute shaders are
  bit-exact in theory, but driver variance makes this a research project, not a foundation.
  GPU results re-enter the engine core only as quantized commands, like AI.

The only artifacts peripherals ever see:

| Artifact | Direction | Shape |
|---|---|---|
| `CommandRing` | in | one SPSC ring per endpoint; protocol in §5.1 |
| `EngineView` | out, with a small return header back | 3 blocks, an exchange word, and a parallel reader-written `ret[3]`; stamped with its tick, row-filtered and converted to float at publish (§3.1, §3.2, §3.4, §3.5) |
| `EventRing` | out | bounded SPSC from the executor to the session's event drain, fanned out per subscriber (§5.2) |

**`CommandRing`** (`design_python_api.md` §7.1):

| Aspect | Rule |
|---|---|
| Endpoint | **one SPSC ring per endpoint**, single-producer by contract, and **one endpoint per source** in v1. A source with several producer threads serializes its own submissions. Source 0 is the host |
| Submission | happens inside the submitting call, on the caller's thread. There is no relaying producer, no buffer-then-flush stage and no contention between sources: two endpoints never touch the same word |
| Result | two per command. **Admission** is returned by the call (`admitted \| queue_full \| too_late \| out_of_order \| over_margin \| invalid \| revoked \| host_error`). The **outcome** is read later: a paced producer reads it for a tick already released, the host polls it, and a logic mod polls or waits. Neither is an event |
| Tick | **a paced submitter names it**, and the command runs at that tick or not at all. Paced endpoints differ only in a declared stamp margin: 0 for an engine source, or a peer's input delay. Unpaced endpoints, the host's and every logic mod's, name no tick: their commands run at the first tick that drains them (§5.1) |
| Capacity | the one number an endpoint declares: how many commands it may hold for **one tick**. There is no drain quota; a tick runs everything stamped for it, because pacing closed the set first |
| Order | the sequence is the position in the drain, so `(source id, sequence)` is a total order **by construction**: assigned by the engine, impossible to forge, and needing no sort |
| Record | recorded as consumed: exactly the commands the tick runs, so what is recorded is what was applied. Rejected commands never enter the record |

**`EngineView`** (§3.1): stamped with its tick, converted to float at publish, and read-only for
every reader except for its return header (§3.5). Publishing is wait-free, one exchange, and
cannot fail. That is a synchronization property only. The projection copy it performs is
budgeted sim-thread work, a sum over registered engine views, bounded per engine view by its row predicate
(§3.4).

**`EventRing`**: from the executor to the session's event drain, bounded, and lossless. The
size is derived, `E × (D + 1)`: a per-tick bound on the events the engine creates, and the
drain interval (§5.2). Every event is an engine event: a command's answers go back to its
caller, and a change in the world reaches readers as state. It cannot overflow, because
the gate stops the executor at the backlog mark, one tick's worth below the size. A source
cannot cause it by submitting. Fan-out to individual subscribers may drop or merge events by
delivery class (`design_python_api.md` §7.3):

| Class | Examples | May drop? |
|---|---|---|
| reliable / audit | a create refused by a declared game-rule cap, a participant dropped at the gate, an async failure, session or replay integrity | never. Inboxes reserve space or keep a sticky loss counter ([Q45](open_question.md#q45-protecting-reliable-events-in-mod-inboxes)) |
| coalescible state | "sim behind", "publish bandwidth over threshold" | only the latest is kept |
| best-effort | telemetry, UI | yes |

Neither of a command's two answers appears in these classes. **Admission** is the submit
call's return value, and the **outcome** is read through the handle it returned (§5.1). So
per-command rejections never enter this ring and never need to be sized for.

How state, commands and snapshots relate:

```
state(N+1)  = tick( state(N), commands drained for tick N )   # only write path
snapshot(N) = projection( state(N) )                          # derived, one-way
```

- The only allowed cycle is snapshot, then decision, then a new command. Nothing flows from
  a snapshot back into state.
- Snapshots are published only at tick boundaries, so a reader never sees a torn, mid-tick
  world.
- Snapshots are projections in three senses: converted (fixed-point to float once, at
  publish), partial by declaration (each engine view's spec, §3.1), and disposable (overwritten,
  latest wins; a missed publish loses nothing).
- Snapshots are **read-only for every reader**, and the publisher is the only writer. They
  are instances of the same generated world container, converted to float and generated
  without any mutating API (`design_data_container.md` §5.1). The publisher's own write path
  still needs a name ([Q40](open_question.md#q40-naming-the-publishers-write-path)). Tier 3
  engine core mods never read snapshots: they access live state inside their phases. Only a mod's
  async peripheral half reads an engine view, like any peripheral. A peripheral that wants writable
  data in the container's shape creates its own float-domain container: the same mechanism,
  a separate instance.
- **A snapshot is not a savegame.** The float conversion loses precision and the projection
  is partial, so engine core state cannot be rebuilt from snapshots. State is rebuilt by replay
  (the full artifact of §2.3, "seed plus command stream" in short). A future save/load
  feature must write the internal fixed-point state bit-exactly, as its own artifact.
- Desync checksums (§2.3) hash internal state, never snapshots. Engine view cadence and content can
  neither cause nor hide a desync.

### 5.1 Command ring protocol (normative)

This section specifies the command ring with the five items of §1.1.

**Two facts support everything below, and they are one mechanism seen from two sides: a
command names the tick it applies to, and its producer is paced.** The gate (§3.3) does not
pass a tick until every source that acts in it has declared ready. A paced producer never
submits and then lets the engine pick a tick: a lower bound is the one answer a submitter
cannot act on.

**Unpaced endpoints are the exception: the host endpoint, and every logic mod's.** In
single-player their commands name no tick. The drain gives each one the tick it runs in: the
first tick whose drain finds it. A command with no tick cannot be late, so nothing has to
wait for its producer. The frame loop never waits for the engine (§3.3), and a logic mod
submits whenever it likes (`design_modding.md` §3). A networked session cannot run them this
way, because every machine must run a command at the same tick. There, a tick-less submit
goes to the machine's turn assembler, which stamps it and sends it to every machine
(`design_multiplayer.md` §3.2).

**An endpoint is single-producer, and a source has one endpoint.** That is a contract, not
an observation. A source with several producer threads serializes its own submissions. Two
endpoints of one source would need a drain order between them, and v1 has no use for one.
In exchange, the inbound path is a plain SPSC ring: no CAS on the slot or index
path, no per-tick cells, no staging arena and no reclamation scheme. And no sort is needed
to order commands deterministically, because the drain order is the order. The endpoint
lease is a separate, cold lifecycle mechanism, because revocation is a second writer the
SPSC indices cannot represent.

**Endpoints declare a stamp margin** at the freeze, next to their capacity. This keeps local
and networked submission on one call with one shape: the difference between an engine
source and a peer is a number declared once, not a second API.

| Margin | Who | A command stamped for a later tick is |
|---|---|---|
| **0** | engine and AI sources. Each acts inside the tick it is about to release | **a bug**, rejected when submitted |
| the **input delay** | a peer, whose commands cross a network and are legitimately in flight across ticks | normal; this is why the margin exists (`design_multiplayer.md` §3.2) |

The transport owns a peer's margin, and its value is open
([Q50](open_question.md#q50-input_delay_ticks)). The engine imposes no value and no bound.
An unpaced endpoint has no margin, because it stamps nothing. A producer with margin `m` may declare ready up to `m` ticks past the current one.
The further ahead it runs, the less often the gate has to wait for it.

**State.** Per endpoint: two hot 8-byte ring atomics on separate cache lines, two cold
4-byte lease atomics, a cold mutex and condition variable for the admission wait, a plain
slot array, and three words touched only by the producer thread.

```
struct Entry { payload…; u64 tick; };        // the tick this command acts on; unused on
                                             //   an unpaced endpoint

struct Endpoint {                            // one per producer, fixed at the freeze
    Entry            slot[DEPTH];            // DEPTH = capacity, or 2 x capacity if margin >= 1
                                             //   (design_limits.md §2)
    alignas(hardware_destructive_interference_size) std::atomic<u64> write;   // producer
    alignas(hardware_destructive_interference_size) std::atomic<u64> read;    // sim
    std::atomic<u32>  access_state;          // OPEN | REVOKING | REVOKED
    std::atomic<u32>  active_submit;         // successful/validating calls in flight
    std::mutex        m;                     // the admission wait's park (§3.3's rule)
    std::condition_variable space_cv;        // a producer waiting for ring space parks here
    u32               space_waiters;         // producers parked on space_cv; guarded by m
    u32               capacity;              // commands this endpoint may hold for ONE tick
    u32               margin;                // 0 for engine sources; the input delay for a peer
    bool              unpaced;               // the host's, or a logic mod's, in single-player
    u64               last_stamped;          // producer-thread-only
    u32               stamped_for_tick;      // producer-thread-only
};
std::atomic<u64> first_unexecuted;           // written only by the sim thread
```

The endpoint object is allocated at the freeze and stays at the same address for the whole
session. Starting a Tier 2 mod host opens its already-assigned endpoint. Quarantine and
retry never swap a new ring into the drain array. Reopening is allowed only after the old
producer has stopped and `active_submit == 0`. Admitted entries already in the ring stay
there and drain before new ones, so the source's FIFO order holds across a retry.

**Endpoint lease.** Every submit call enters the endpoint before touching its ring:

```
try_enter:
    if access_state.load(acquire) != OPEN: return revoked
    active_submit.fetch_add(1, acq_rel)
    if access_state.load(acquire) != OPEN:
        active_submit.fetch_sub(1, release); notify revoker; return revoked
    return LEASED                                      # LINEARIZATION POINT

leave:
    if active_submit.fetch_sub(1, release) == 1: notify revoker

revoke:
    { std::lock_guard g(gate.m); std::lock_guard lk(m);    # both parks read access_state
      access_state.exchange(REVOKING, acq_rel);        # closes new leases
      space_cv.notify_all();                           # an admission wait is not a lease leak,
      gate.progress_cv.notify_all(); }                 #   nor is an outcome wait (§3.3)
    wait under the shutdown deadline for active_submit == 0
    access_state.store(REVOKED, release)                # revocation complete

open_or_reopen:
    require old producer stopped && active_submit.load(acquire) == 0
    access_state.store(OPEN, release)                   # do not reset ring indices/slots
```

A lease that linearized before `REVOKING` completes normally, including publishing its entry
after revocation began; revocation waits for it. A later call returns `revoked` before
touching an index or slot. Revocation also **wakes every parked producer**: one asleep on
the admission wait, and one waiting for an outcome. A producer asleep on the admission wait
holds a lease, and would otherwise be waited on for the whole shutdown deadline; it wakes
with `revoked` and leaves. An outcome waiter wakes with `revoked` too. Revocation is the
only thing that wakes either wait early. The change of `access_state` happens under both
mutexes, as §3.3's wake-up rule requires of anything a parked thread waits on. The lock
order is always `gate.m` before an endpoint's `m`.

The endpoint and its control storage stay alive until the producer has been joined. If the
deadline expires, the engine follows the existing `failed` and disarm path instead of
freeing memory under a caller. Polling `ctx.stopping` is only advisory: seeing `false` and
then getting `revoked` is legal, because revocation may happen between two calls. The submit
result is the only admission fact a caller acts on (`design_modding.md` §4.1). A scope guard
calls `leave` on every exit after the lease, including validation failure, `queue_full`, a
woken wait, and exceptions.

**Submit** runs on the caller's thread. Admission is a wait, not a coin flip: a producer
waits for space, and fails only by asking for more than it declared.

```
submit(cmd, t):
    lease = try_enter(); if lease == revoked: return revoked           # (C0)
    fu = first_unexecuted.load(acquire)
    rt = my_participant.ready_through.load(relaxed)                    # our own declaration
    if t < fu || t <= rt:  leave(); return too_late                    # (C1a) past, or released
    if t <  last_stamped:  leave(); return out_of_order                # (C1b)
    if t >  fu + margin:   leave(); return over_margin                 # (C1c)
    if t != last_stamped:  last_stamped = t; stamped_for_tick = 0      # producer-local
    if stamped_for_tick == capacity: leave(); return queue_full        # (C1d)
    w = write.load(relaxed)                                            # (C2) our own word
    if w - read.load(acquire) == DEPTH:                                # (C3) THE ADMISSION WAIT
        std::unique_lock lk(m); ++space_waiters                        #      re-check under m
        space_cv.wait(lk, [&] { return w - read.load(relaxed) < DEPTH
                                     || access_state.load(relaxed) != OPEN; })
        --space_waiters
        if access_state.load(relaxed) != OPEN: leave(); return revoked
    slot[w % DEPTH] = cmd; slot[w % DEPTH].tick = t                    # (C4) plain; the slot is ours
    ++stamped_for_tick
    write.store(w + 1, release)                                        # (C5) LINEARIZATION POINT
    leave(); return admitted{ handle }
```

**When the admission wait happens.** A ring at margin 0 holds `capacity` entries, all for
the one tick a margin-0 producer may stamp, and (C1d) refuses the entry past `capacity`. So
a margin-0 ring never fills while its producer stays within its allocation, and (C3) never
parks there. An endpoint with a margin of 1 or more holds `2 × capacity`: two ticks' worth,
whatever the margin. A producer running further ahead than that fills its ring and parks
at (C3) until the oldest tick in it has run. That is what the wait is for: a fast producer,
such as a peer's receive thread with several turns already delivered, waits for space
instead of failing.

**Why it cannot deadlock.** A paced producer stamps the tick after its last declaration,
`ready_through + 1`: it submits for a tick, declares it, then moves to the next. (C1a)
refuses a tick it already declared, and stamps never decrease (C1b). The ring holds at most
`capacity − 1` entries for the tick being stamped, or (C1d) would have refused the submit
first. So a full ring holds more than `capacity` entries for earlier ticks, all of which
this producer has already declared. Those ticks do not wait for it, and the drain frees
their space without its help. A producer that stamps further ahead before declaring breaks
this order. Its ring can then fill with ticks it still holds, and the wait ends only when
its participant deadline expires (§3.3).

**How long it lasts.** Until the oldest tick in the ring has run. Normally that is at most
one tick. While the gate is held, by a pause, the event backlog or another participant, it
lasts as long as the gate does. The wait has no timer of its own; its bound is structural.

**An unpaced submit names no tick and never waits.** On the host endpoint the producer is
the owner thread, which must not park (§3.3), and a logic mod's producer gains nothing from
parking either: it holds no tick, so nothing else is waiting for it. So an unpaced submit
has no stamp checks and no admission wait:

```
submit_unpaced(cmd):                                                   # host or logic mod
    lease = try_enter(); if lease == revoked: return revoked           # (H0)
    w = write.load(relaxed)
    if w - read.load(acquire) == DEPTH: leave(); return queue_full     # (H1) never waits
    slot[w % DEPTH] = cmd                                              # (H2) plain; no tick
    write.store(w + 1, release)                                        # (H3) LINEARIZATION POINT
    leave(); return admitted{ handle }
```

Its depth is its capacity: the most commands that may wait between two drains. Every drain
takes all of them, so a full unpaced ring means its producer submitted more than `capacity`
commands before the next tick ran.

`submit_batch` is `k` submits and needs nothing else. An endpoint has one producer, so `k`
entries from it are always contiguous in the ring, and ordering the array orders the
commands.

**Drain** runs on the sim thread at the start of tick `t`, over endpoints in **ascending
source id**:

```
seq = 0
for e in endpoints (ascending source id):
    r = e.read.load(relaxed); w = e.write.load(acquire)       # (S1)
    while r != w:
        st = e.unpaced ? t : slot[r % DEPTH].tick             # unpaced entries run at t
        if st > t: break                                      # (S2) stamped for a later tick
        if st < t: protocol_error(e, st, t)                   # (S3) NEVER executed late
        emit(slot[r % DEPTH], source(e), seq++); ++r
    if r != e.read.load(relaxed):                             # freed space: §3.3's rule
        std::lock_guard lk(e.m)
        e.read.store(r, release)                              # (S4)
        if e.space_waiters: e.space_cv.notify_all()           # (S5) only a parked producer
record; execute                                               # §2.3, §6; then publish, and
                                                              #   first_unexecuted advances
                                                              #   at (G5) (§3.3)
```

**Everything stamped for `t` runs at `t`. There is no quota and no per-tick limit on the
count.** That is what makes the drain deterministic, not just bounded. Pacing closes the set
before the drain runs. When the gate opens for `t`, every participant has already submitted
its commands for `t` and declared ready. Anything submitted afterwards is stamped `t + 1` or
later, because `first_unexecuted` has not moved. **The tick label makes the cut, not a
count**, and a count is exactly what two machines could disagree about. An unpaced endpoint
has no label, so its cut is the index the drain loads at (S1): every entry visible there
runs at `t`, and a later one runs at the next tick.

Five properties follow from the mechanism:

- **The drain never waits for a producer.** (S1) reads whatever index the producer has
  published and stops there. The engine's wait for that producer is the gate's: declared,
  and finished before the drain ran (§3.3). No slot is ever reserved but unfilled, because a
  slot becomes visible and complete in the same operation, (C5).
- **A paced producer waits only for space, never for a tick it holds.** (C3) is the only
  interaction, and it lasts until the oldest tick in the ring has run, a tick this producer
  has already released (above). The host never waits at all: (H1) returns `queue_full`
  instead.
- **Ordering is total, deterministic and cannot be forged.** The drain assigns `seq` in a
  fixed endpoint order over each endpoint's FIFO. So `(source id, sequence)` is a total
  order with no sort and no field a source could forge. Order within one source is the
  producer's own.
- **A command runs at the tick it named, or not at all.** (S2) stops the drain at the first
  entry for a later tick. (S3) refuses to run one whose tick has passed. There is no third
  outcome where a command silently moves. A host command names nothing, so it runs at the
  tick that drains it.
- **An entry stamped for the future keeps its place, and nothing behind it is pulled
  forward.** This needs stamps that never decrease, and (C1b) enforces that rather than
  trusting a well-behaved peer, because the failure would be invisible. A command stamped
  `t+5` queued ahead of one stamped `t+1` would make the second run at `t+5`, silently, on
  that machine alone. (S3) turns any violation that gets past the submit check into a loud
  protocol error instead of a late execution. So the property is checked at both ends, and
  no deferral structure is needed.

**Two waits, and they are not interchangeable.** Submitting answers one question. What the
command did answers another, and that answer does not exist until the tick runs.

| | **Admission**: is it in the queue? | **Outcome**: what happened when it ran? |
|---|---|---|
| Who | every paced producer, peers included | every paced producer |
| Why | the alternative is dropping a command, and for a peer a dropped command is a certain desync | feedback, and the only place a rejection at consumption is reported |
| Length | until the oldest tick in the ring has run | until the named tick has run |
| Can it fail? | only if the producer breaks its own contract | **no.** It *reports* failures; it is not one |

```
h = submit(cmd, t)        # admission: it is in the queue for t
...
outcome(h)                # what it did at t: applied, or a named application-time rejection
```

For a peer the outcome is worth having even though it is identical on every machine: it
shows trouble early, and it gives the transport a way to push back.

**An unpaced producer gets both answers without having to wait for either.** A full
unpaced ring returns `queue_full` (H1). `outcome(h)` returns `pending` until the tick that
drained the command has run, then the outcome together with that tick. The frame loop polls
it (§3.3). A logic mod may poll it or wait for it; it holds no tick, so the wait cannot
deadlock.

**The deadlock rule.** A deadlock can happen only if something waits for the outcome of a
tick it is itself holding up. The right order of operations removes it:

```
submit commands for tick t     # admission
declare ready for tick t       # releases the tick
...the engine drains, runs, advances...
read outcomes for tick t       # a tick already released
```

**A paced producer may wait only for outcomes of ticks it has already released.** The
engine enforces this rather than just documenting it. It knows which participant is asking
and which tick that participant holds, so a wait for the outcome of an unreleased tick is
refused at once, naming the tick. A clear error at the call site is far better than a frozen
session. An unpaced producer cannot break the rule: it holds no tick, so nothing it waits
for depends on it.

Admission cannot deadlock either: a full ring always holds ticks its producer has already
released, and the drain frees them without the producer's help (above). (C1d) marks the
producer's own allocation, and crossing it is the contract violation, not the wait.

**There is exactly one capacity rejection.** `queue_full` means "more than you allocated for
this tick": a producer breaking its own declaration, not a busy engine. For a paced
producer, a busy tick, a slow drain or a paused session causes a wait, not a rejection. So
its every rejection is either a contract violation, a lifecycle state, or a transport
failure, and none is a load signal. Unpaced endpoints are the exception: they never wait,
so a sim that is paused or behind lets them fill, and their `queue_full` is the one load
signal, which the producer reads:

| Result | Class | Means |
|---|---|---|
| `admitted{handle}` | none | in the ring, for the tick you named. The handle reads the outcome |
| `queue_full` | contract | more than `capacity` commands stamped for one tick on this endpoint. On an unpaced endpoint: more than `capacity` commands waiting for the next drain |
| `too_late` | contract | the named tick is in the past, or this producer has already declared it ready |
| `out_of_order` | contract | the named tick is earlier than one already named on this endpoint |
| `over_margin` | contract | further ahead than this endpoint's declared margin; for a margin-0 producer, any tick but the current one |
| `invalid` | contract | malformed payload, or a capability the source does not hold |
| `revoked` | lifecycle | this endpoint is not admitting: torn down, or the session is in playback |
| `host_error` | transport | a process host's IPC round trip failed or exceeded `ipc_deadline`; process hosts only (`design_modding.md` §4.3) |

An unpaced endpoint names no tick, so `too_late`, `out_of_order` and `over_margin` cannot
arise on it.


**`too_late` rejects a tick that is past, or that this producer has already released.** The
tick the engine is currently waiting on is not late, and it is the one nearly every command
lands in. `first_unexecuted` has not moved and cannot, because the producer that would
release that tick is the one submitting. Pacing, not timing, makes the current tick safe. A
peer's commands for `t` arrive before it declares ready for `t`, and the gate cannot pass `t`
until it does.

A tick the producer has already declared ready is refused even if it has not run yet. The
declaration said "I have finished submitting for `t`". Once every other participant has
said the same, the gate may pass `t` at any moment. A command admitted for `t` after that
could miss `t`'s drain and reach (S3) as a protocol error. Refusing it at submit, where the
producer is still there to be told, turns that race into a returned result. So in a healthy
session `too_late` cannot happen; if it does, it reports a defect, not load.

**Capacity is the only number an endpoint declares:** how many commands it may hold for one
tick. There is no second number for the drain to cap, because a tick runs everything stamped
for it. The ring depth follows:

| Endpoint | Depth |
|---|---|
| margin 0 | `capacity` |
| margin 1 or more (a peer) | `2 × capacity`: the tick being filled, and one already released. A producer further ahead waits at (C3) |
| unpaced: the host endpoint, a logic mod's | `capacity`: every drain empties it |

The engine-wide per-tick ceiling is `C = Σ capacity(e)` over registered endpoints
(`design_limits.md` §2). It is computed at the freeze, and nothing is sized from it: each
ring is sized from its own endpoint's capacity. That is why the limit on sources can be
generous at no cost to a session with four sources. The event ring's size does not depend
on it either, because no command's answer travels on the event ring (§5.2).

**Memory orders.**

- (C5) `release` pairs with (S1) `acquire`: one edge per publish, making every entry up to
  `w` visible.
- (S4) `release` pairs with the `acquire` load in (C3), or with the endpoint mutex when the
  producer re-checks under it. This makes a slot safe to overwrite: the sim's reads of it
  happen before the producer's next write to it.
- The admission wait follows the wake-up rule of §3.3. The drain stores `read` under the
  endpoint's mutex and notifies a parked producer (S4, S5). The producer re-checks under the
  same mutex before it sleeps (C3). So a drain that frees space either lands before the
  re-check, which sees it, or after the producer is parked, and then notifies it. Nothing
  depends on `seq_cst`.
- (C1a)'s load of `first_unexecuted` is `acquire`, pairing with its `release` store at (G5)
  (§3.3), which the executor makes under `gate.m` after the tick's publish.
- (C2), (C4) and each side's load of its own index are relaxed or plain.
- On an unpaced endpoint, (H3) `release` pairs with (S1) like (C5), and (H1)'s `acquire`
  load of `read` pairs with (S4). Nothing parks there, so nothing needs a wake-up, and the drain
  may store its `read` without taking the mutex.

**Progress.** An uncontended submit is wait-free: two lease loads, two active-count
read-modify-writes, three bounded validity checks, one relaxed index load, one acquire
load, a copy and one release store, with no loop. The drain takes the endpoint's mutex only
when it frees space there: one short lock per active endpoint per tick, a few microseconds
per tick for 256 endpoints. A submit that finds the ring full is a
**bounded wait** in the sense of §1.1: declared, bounded by the oldest tick in the ring
running, and with no expiry on the normal path. The only other way out is revocation, which
wakes it with `revoked`. The drain is `O(commands stamped for this tick)` with no
retries. The ring indices are never contended, since the producer owns `write` and the sim
owns `read`. Revocation contends only on the separate endpoint lease. An unpaced submit is
wait-free in every case, full ring included.

**Failure and deadline behaviour.** Every rejection in the table above is returned
synchronously, and the command is not enqueued. The admission wait has no deadline of its
own, on purpose: its bound is structural (the oldest tick in the ring running), not a
timer. A session that is not advancing is paused or held at the gate, and the waiter waits
with it (§3.3). Revocation waits, under the shutdown deadline, for leases that already linearized.
If that deadline expires, the engine enters the terminal `failed` state and disarms instead
of reclaiming the endpoint.

**Verification.** The concurrency test is a sanitizer test. It checks that every command
that returned `admitted` appears exactly once in the drained stream, in submission order
within its endpoint, and at the tick it named: never earlier, never later. Three races must
be tested:

- **Revoke**: calls whose lease came before `REVOKING` may be accepted exactly once. Every
  later call returns `revoked` and touches no ring memory. A producer parked on the
  admission wait is woken rather than waited out. Reclamation never happens before running
  calls finish.
- **Full ring**: a producer parked at (C3) is woken by the drain that frees its slot, and
  never sleeps through one.
- **Pause**: a pause taken while producers are parked wakes none of them. The resume that
  lets the oldest tick run frees their space, and the drain wakes them.

The wake-up parts of these races (a parked producer woken by revocation or by a drain) are
hangs, not data races, so the sanitizer does not see them. They are the stress
tests of §3.3, run under a watchdog timeout.

### 5.2 Event ring (normative)

The event ring is a bounded single-producer, single-consumer ring. The executor, the thread
that runs ticks (§3.3), enqueues. The session's event drain empties it (below). Publication
uses the idiom of §1.1: fill the entry, then release-store the write index. The drainer's
acquire load of that index makes the entry visible. It has the same shape as the ring of
§5.1, with the direction reversed.

**Only the engine raises events.** An event reports on the engine itself: the session, the
gate, participants, endpoints, engine views and caps. **A change in the world is never an
event.** A reader sees it as state, in its engine view (§3.1). A command's two answers go
back to its caller and never enter the ring: admission is `submit`'s return value, and the
outcome is read from the command's handle (`design_python_api.md` §7.1). So neither a source
nor a system can fill the ring, and its bound is a count the freeze closes (below).

**A reader that must not miss a change reads it from state.** An engine view holds the
latest publish, and an observer may fall any distance behind (§3.3). So a change that is
undone before the reader's next snapshot never reaches it. A change that a reader must not
miss is kept in the world instead: a counter, a column holding the tick of the last change,
or rows of a log object type that systems create and later erase. A reader that needs every
tick paces its engine view, and the gate then waits for it, up to its deadline (§3.3).
Which of these the engine supports directly is open
([Q82](open_question.md#q82-how-does-a-reader-learn-of-a-world-change-it-must-not-miss)).

**Every session declares one event drain.** The drain says who empties the ring. The freeze
refuses a session that declares none (`design_python_api.md` §3), because a ring nobody
empties stalls the session while it does nothing else wrong.

| Drain | Who empties the ring | For |
|---|---|---|
| **owner drain** | the owner thread, in `drain_events()`, between ticks and never inside one (`design_python_api.md` §6) | a session whose application or mods read events: every windowed session |
| **sink drain** | the executor itself, after every tick, inside a `step(n)` too. The sink is a built-in native consumer: it counts the events and hashes their canonical encoding, and keeps nothing | a run nobody reads events from: CI, golden replays, benchmarks |

Under a sink the backlog never grows, so `EVENT_BACKLOG` never blocks, and `drain_events()`
is refused: the ring already has its one consumer. The sink's count and hash are what a
golden replay can compare.

**Why it has exactly one producer.** The backlog rule below depends on this. Events are
emitted at commit points, and commit points are single-threaded: the staged merge at every
phase close and the terminal commit both run sequentially in fixed task order (§4.1). That
order is the determinism guarantee, so it cannot be relaxed. The terminal commit's erase and
create steps emit only the cap-refusal events of `design_data_container.md` §2.2.
Anything that needs to report from inside a parallel phase uses the same route as log
records. It writes to a per-worker buffer, which the single thread running the phase
boundary drains in worker order (`design_patterns.md` §7). So the ring has one producer by
construction, not by convention, and no emission site needs auditing for it.

**Events raised outside a tick never enter the ring.** `session.started` is raised by the
freeze, and `participant.left` by a mod host stopping (§3.3), on the owner thread or a mod
host thread. They wait in a short side list under its own mutex, and the drain takes them
with the ring's events, in tick order. So the ring keeps its one producer, and these events
need no room in it.

**The ring's size and the backlog mark are derived.** One tick emits at most `E` events into
the ring, because every event is an engine event (above). `E` is computed at the freeze from
counts the freeze closes (§2.4):

| Quantity | Formula | What each term is |
|---|---|---|
| `E`: engine-created events per tick | `T·S + S + 3·P + V + 4` | `T·S`: one cap refusal per object type and source (`design_data_container.md` §2.2), from M3. `S`: one protocol-error report per endpoint (§5.1). `3·P`: expired, suspended and resumed, per participant. `V`: one bandwidth warning per engine view (§3.1). `4`: `session.stopped`, `sim.backlog_paused`, `sim.backlog_resumed` and `sim.behind` |
| `high_water` | `E × D` | `D` drain intervals' worth of events: the backlog at which the gate pauses the sim |
| ring size | `E × (D + 1)` | the mark plus one tick of headroom |

`T` is the number of object types, `S` of sources, `P` of participants and `V` of engine
views. The backlog is checked only at the gate, before a tick. A tick that passes just below
the mark adds at most `E` entries, which the extra tick of headroom holds. **So the ring
cannot overflow:** the gate stops the executor before any tick that could. The event list
itself is in `design_python_api.md` §7.3, and a new engine-created event adds its term to `E`
in the same change.

**The event backlog has one owner.** `backlog` counts the entries the drain has not yet
consumed. It has exactly one writer, the executor, which is the only thread that adds
entries. It is a plain `u64`, not an atomic and not a flag, because the other thread never
touches it:

```
enqueue:           ++backlog                                # executor, plain
blocker_for(t):    backlog = write - read.load(acquire)     # recomputed at every gate check,
                   if backlog >= high_water: EVENT_BACKLOG  #   the locked re-check (G2) too
```

The drain publishes nothing new for this. `read` is the index it already stores as the
ring's consumer. The backlog is recomputed from two indices that only increase, at the one
place the executor already evaluates the gate.

**How a drain wakes a parked sim.** This matters only with a sim thread (M6). An owner
drain's `drain_events()` consumes its batch, then takes `gate.m` once: it stores `read`, and
notifies the sim only if `gate.parked == EVENT_BACKLOG`. The sim recomputes the backlog in its
locked re-check (G2), from the `read` the drain stored under the same mutex. So the wake
follows the wake-up rule of §3.3: a drain lands either before the re-check, which sees the
freed space, or after the sim has parked, and then wakes it. The cost is one short lock per
drain call, once per frame. The mechanism has its own row in §1.1.

**What happens at the mark depends on who would wait.**

| Executor | Drain | At `high_water` |
|---|---|---|
| any | sink | never reached |
| the sim thread | owner | the sim parks on `EVENT_BACKLOG`, with no deadline, like a pause. The frame loop's next drain wakes it |
| a `step(n)`, on its calling thread or on a paused sim thread | owner | the owner thread is the one that must drain, and it is inside `step(n)`. So the step raises `EventBacklogError` at the tick boundary, naming the tick reached, instead of waiting. The caller drains and steps again (`design_python_api.md` §4.3) |

**Why a derived value and not a cached flag.** A flag looks cheaper, but cannot be made
correct. Both threads would have to write it: the enqueue sets it and the drain clears it,
and each writes based on a count read before the other wrote. The drain clears it. An
enqueue holding a count from before the drain sets it again. The engine then blocks on a
backlog that does not exist, until some later enqueue happens to re-evaluate it. A recheck
fixes one direction and not the other, because two writers deciding one fact from two stale
readings is a mechanism with two owners, not a race to patch. One owner of a derived value
removes the whole class of bug.

**Overflow: who is at fault decides the response.** These three cases are the engine's whole
overflow policy. They are kept apart so that the mildest fault does not get the harshest
response:

| Queue | At fault | Response |
|---|---|---|
| a mod's own inbox | that mod is not draining | **suspend that mod** and report; the simulation continues (`design_modding.md` §4.2) |
| the event ring | the application is not draining | **pause the simulation** and report: `EVENT_BACKLOG` above |
| a peer falling behind | never decided locally; the server tier decides (§3.3, `DROP`) | until the server tier decides: **pause and report** |

None of these ends a session. Each pause is undone by whatever caused it: the application
drains and the gate opens, or the server tier answers and the drop applies at its agreed
tick. **Suspension is reversible too.** The mod stops being fed, keeps its endpoint, and
resumes once it drains (`design_modding.md` §4.2). Unloading a suspended mod is a separate,
deferred question ([Q47](open_question.md#q47-unloading-a-suspended-mod)).

`D` means the same in every session, and the ring's size still holds under fast-forward.

## 6. Agent-based systems: hybrid mind/body split

An agent has a **body**: position, resources, health, anything the world tracks. The body is
engine core state and updates deterministically. Where the agent's **mind** lives is a choice of
determinism boundary, and it decides what a replay needs:

- **Mind outside the boundary** (an async float or GPU brain that emits commands): the
  simulation is deterministic given the recorded command stream, but cannot be reproduced
  from the seed alone. Agent commands must be recorded exactly like player input. Replay
  size grows with agent count times decision rate, and lockstep multiplayer would need an
  authority to compute and broadcast agent commands.
- **Mind inside the boundary** (fixed-point decision logic, engine-core-seeded RNG, deterministic
  decision ticks): replay from the seed works with nothing recorded per agent. But no float
  math is allowed, and GPU inference inside the boundary is impractical (driver variance,
  §5).

**The design is a hybrid:**

- **Inside the engine core (deterministic)**: cheap, frequent, per-tick agent logic such as utility
  scoring, steering, state machines, pathfinding and reflexes. This is simple math anyway,
  runs in parallel under §4 like any other system, and keeps replays small.
- **Outside, as peripherals (float or GPU, async)**: expensive, occasional thinking such as
  planning, learned policies and batched neural-net inference. These read snapshots, think
  at their own pace, and emit commands that are recorded into the input stream.
  Architecturally this is the same as a human player: a non-deterministic brain whose
  signals are deterministic inputs.

Rules that keep this sound:

- **The engine core resolves agent commands deterministically**: validated, ordered by `(source id,
  sequence)`, and conflicts settled by fixed rules, never by arrival time.

  | Aspect | Rule |
  |---|---|
  | order key | `(source id, sequence)`, both **assigned by the drain** (§5.1): the endpoint order and the position within it. Nothing is sorted, because nothing arrives out of order |
  | why the engine assigns it | a sequence supplied by the source need be neither unique nor increasing, so ties would resolve by arrival time: an iteration-order desync (§2.2) disguised as a sort |
  | order within one source | still the source's own: an endpoint is a FIFO, so `submit_batch` is `k` submits and ordering the batch orders the commands |
  | naming | an **id**, never a row. A snapshot row from tick N means nothing at tick N+k, and a bare slot cannot tell a new occupant from an old one (`design_data_container.md` §2.2) |
  | resolution | id to row in O(1): the id's slot is the row, and the entity is alive only if the slot is live and its generation matches (`design_data_container.md` §2.2). **If the id is not found, the entity is gone** and the command is deterministically rejected. That is reaction latency, never an error or undefined behaviour |

  The last row is a positive identity test: a command can never silently apply to a
  different entity from the one it named.
- **A paced submitter names the tick a command applies at, and the engine either keeps
  that promise or refuses the command** (§5.1). Nothing is moved to a different tick. The
  drain runs what is stamped for the current tick, refuses what is stamped for a tick
  already gone, and leaves a later stamp alone. So a caller is never told a tick that later
  changes.

  The gate makes this more than bookkeeping. Every instance waits for `t` rather than
  running past it (§3.3), so a named tick means the same tick everywhere. A lockstep peer
  needs this (`design_multiplayer.md` §3.2). How far ahead of the current tick an endpoint
  may name is its declared stamp margin: 0 for an engine source, the input delay for a peer
  (§5.1). The transport chooses that delay; the engine imposes no value and no bound. The
  host and logic mods name no tick at all: their commands run at the tick that drains them,
  and the record says which (§5.1).
- **Replay feeds commands in at consumption, not at submission.** The recorded stream is
  already ordered and already carries each command's tick (§2.3, §5). So a loaded replay
  hands each tick its commands directly and closes every endpoint while it runs: every live
  submission, from every source, is refused (`design_python_api.md` §4.2). Otherwise live
  commands could be mixed into a replayed stream, and the checksum would diverge for reasons
  nothing records.
- **Async minds working from a tick-N snapshot get natural reaction latency.** Slow
  inference costs the agent responsiveness rather than costing the simulation its tick
  rate. A mind outside the boundary is a logic mod (`design_modding.md` §3): it names no
  tick, its command runs at the next drain, and the session never waits for it. A decision
  that must act at an exact tick belongs inside the boundary, as engine core logic or a Tier 3
  mod. A mind that only watches submits nothing and paces nothing. In a networked session a
  logic mod runs once, on a mod client (`design_multiplayer.md` §4.2).

## 7. Suggested build order

Each step is labelled with the milestone it belongs to (see `glossary.md`).

1. **M1: session lifecycle, tick loop, the gate, command rings and one engine view**, with no
   simulation content. The `configuring → freeze → running` sequence (§2.4) belongs here
   rather than later. Identity, column ids, source ids, engine views and participants are all fixed
   at the freeze, so every later step is written against a session that already exists. M1
   runs headless: `step(n)` runs ticks on the calling thread, and the host is an unpaced
   source. The gate then ends a step at its grant, stops it at the event backlog mark, and
   waits only for paced engine views (§3.3), so this step builds the gate at its simplest
   setting.

   **Until the world exists, M1 carries opaque bytes.** Columns arrive in step 3 and
   projection in step 5. Until then a block holds its tick, a size and plain bytes; its
   engine view declares how many at registration. The engine publishes no bytes. Tests fill
   them, to check that a block arrives whole. A command ring entry holds its tick and
   payload bytes of a provisional size
   ([Q22](open_question.md#q22-how-large-is-one-command-or-event-slot)). The one command
   does nothing, and its outcome is `applied`, with the tick that ran it. `snapshot()`
   returns the tick alone.
2. **M2: the `fixed<>` type, deterministic PRNG, the three checksum levels (§2.3), and
   replay record and playback.** The verification harness must exist before the first
   system does.
3. **M3: the SoA world container**, with chunked columns, generational ids over stable
   slots, and SIMD-friendly alignment.
4. **M4: the phase-structured scheduler**: explicit system list, per-column access
   declarations, the phase cut and its invariant check, staged buffers, fixed-order commit.
   Prove that 1 thread and N threads give the same checksums.
5. **M5 and M6: the first real system, and a renderer reading an engine view with the `ALL`
   predicate**, with windowed sessions running their ticks on the sim thread
   (`design_python_api.md` §4.3); then grow. A row predicate (§3.4) is added when `publish bytes/second` calls
   for one, not before.

## Appendix A. The `SHARED` engine view (deferred)

**Not built in v1.** This appendix specifies the multi-reader engine view mode, kept because the
analysis is sound and one kind of reader will need it. It describes the design in the
present tense; nothing here is built until process-host mods are.

**Why it is deferred.** A `SHARED` reader cannot keep a block, so it copies out. N readers
therefore cost N full copies of the projection, on top of the publisher's one. At the row
counts `design_limits.md` §5 budgets for, that is the largest cost in the whole engine. One
`SHARED` engine view with three readers over 10⁷ rows at a 32-byte spec moves 1.28 GB per tick.
Three `PRIVATE` engine views over their own declared columns and rows move a fraction of that. The
mode named for sharing is the one where everyone copies.

**Why the copy is unavoidable.** §1.1 allows no reclamation on a publish path: no hazard
pointer, no payload reference count, no deferred reclamation. GCC 16 provides neither
`<hazard_pointer>` nor `<rcu>`, so adopting one would mean writing the scheme ourselves.
Without deferred reclamation, a reader may not hold a block pointer across publishes. So its
pin must be brief, and so it must copy while pinned. The copy is the price of refusing
reclamation, and the pin count is itself a very short-lived reclamation scheme.

**This reasoning reverses at scale**, which must be checked before reviving the mode as
written. Copying out 320 MB takes roughly three quarters of a 33.3 ms tick. So the pin the
copy exists to keep short is held for most of a tick anyway, and `publish_skipped_pinned`
becomes a permanent condition rather than a diagnostic. A revival should compare a
per-reader epoch slot over the registered reader set with the pin count
([Q60](open_question.md#q60-reviving-the-shared-engine-view)). Readers are known at the freeze,
which removes most of what makes general hazard pointers hard.

**What needs it.** A process mod host, which cannot be given a `PRIVATE` engine view: a `PRIVATE`
publisher uses the block index its reader wrote (§3.2), so a hostile reader could steer a
payload address. `SHARED` reads the block-state words the child process writes only as
availability, and never derives an address, index, epoch or loop bound from them. The
shared-memory mapping, the permission split and the fixed interprocess atomic ABI are in
`design_modding.md` §4.3, which is deferred together with this appendix.

**Mode comparison**, against §3.1's `PRIVATE`:

| | `PRIVATE` (v1) | `SHARED` (deferred) |
|---|---|---|
| Readers | exactly one, trusted | any number |
| Blocks | **3** | **4** |
| Take | one `exchange`: the reader hands back its old block and receives the newest | pin the published block, revalidate, **copy out**, unpin |
| Retention | the reader keeps its block **as long as it likes, without copying** | the copy is the retention |
| Reader progress | wait-free | lock-free (a failed revalidation retries) |
| Return header (§3.5) | yes | **no**; see "Trust" in §3.5 |
| Who may own one | engine code, the host, a thread-host mod | anyone, including a mod **process** |

**Why four blocks.** With four blocks, pin pressure shows up only with a truly slow reader.
The current block stays available to new readers while the publisher scans the other three,
and each publish can leave one older epoch pinned without waiting. Four blocks give
**three epochs of slack**. This is not an assumption that a copy finishes within 66 ms:
an arbitrarily slow copy stays safe, and if all three candidates are pinned, that engine view skips
one due publish.

**State.**

```
struct SharedView {
    Block*            block[4];
    std::atomic<u64>  published;      // (epoch << 2) | index
    std::atomic<u32>  state[4];       // bit 31 WRITING; low 31 bits reader count
    u32               cursor;         // publisher-private scan start
    u64               epoch;          // publisher-private; 62 bits on publication
};
```

The cursor and epoch are plain variables touched only by the publisher. Running out of
epochs is a terminal invariant failure, checked before wrap. At 30 Hz, 2^62 publishes
is far beyond any possible session, but the rule removes ABA outright rather than relying on
that. Block states are 4 bytes and the published word is 8. No 16-byte atomic and no tagged
pointer is allowed.

**`SHARED`**: a reader count pins ordinary memory; `WRITING = 1u << 31`.

```
publish:
          for i in the 3 non-current blocks, once:          # (S1) bounded scan
              expected = 0
              if state[i].compare_exchange_strong(
                     expected, WRITING, acquire, relaxed):  # (S2) exclusive claim
                  chosen = i; break
          if no chosen: ++publish_skipped_pinned; return SKIPPED
          commit/replace *block[chosen] if needed            # (S3) claim excludes readers
          fill *block[chosen]                                # (S3) plain
          state[chosen].store(0, release)                    # (S4) payload/ptr complete
          ++epoch; assert(epoch < 2^62)
          published.store((epoch << 2) | chosen, release)    # (S5) LINEARIZATION POINT

read:
retry:    a = published.load(acquire)                       # (R1)
          i = a & 3
          s = state[i].load(relaxed)
          if (s & WRITING || (s & ~WRITING) == COUNT_MAX): goto retry
          if (!state[i].compare_exchange_strong(
                   s, s + 1, acquire, relaxed)): goto retry # (R2) PIN
          b = published.load(acquire)                       # (R3)
          if (b != a): state[i].fetch_sub(1, release); goto retry
          p = block[i]; copy *p                             # (R4) plain; pinned
          state[i].fetch_sub(1, release)                    # (R5) UNPIN
```

**Why validation happens before the copy.** Loading a pointer or payload and then checking a
version is too late: the overlap has already formed a C++ data race. Here a stale reader
either loses (R2) to the writer's claim, or pins first and makes (S2) fail. After the pin,
an exact match at (R3) shows that the pinned block is still the published epoch. Only
then is the pointer loaded. A retry therefore detects a lost publication race before any
ordinary payload access; it never tries to excuse a torn copy.

**Memory orders.** (S4) and (S5) release the pointer and payload; (R1), (R2) and (R3)
acquire them. A reader's (R5) release pairs with a later successful writer (S2) acquire, so
the whole copy happens before the block is replaced or refilled. Failed relaxed CAS
operations publish nothing and touch no payload.

**Progress.** Publish is wait-free: at most three strong CAS attempts, then publish or skip.
Read is lock-free: a failed pin means a writer or another reader changed that state, and a
failed revalidation means the publisher advanced. A saturated count also retries until a
reader releases, which is progress for the system.

**Failure and deadline behaviour.** A read cannot fail. A publish may return `SKIPPED` for
that engine view after its bounded scan, leaving the previous immutable snapshot published. This is
sampling, not a session failure, and emits no event per skip. `publish_skipped_pinned`
counts skips, and a sustained nonzero rate identifies a reader holding its pin too long. A
`SHARED` engine view may not be paced: it is sampled and may skip, so `paced=True` is a
registration error rather than a promise the mechanism cannot keep.

**Committing and replacing.** A successful `0 → WRITING` claim proves that no reader can
have loaded that block's pointer. So the publisher may commit pages for the block, or free
and replace it in place. A reader loads
`block[i]` only after pinning and revalidating, so the pointer is never read and written at
the same time.

**Verification.** The sanitizer test checks three things. No ordinary pointer or payload
access happens without a successful pin and revalidation. A writer never claims a block with
a nonzero reader count. And when all three candidates are pinned, the publish skips without
touching a pinned block. A long reader spanning many publishes, and a race between
committing pages and replacement, are required cases. A protocol that validates after copying cannot express
them without undefined behaviour.

## References

- [Gaffer On Games — Deterministic
  Lockstep](https://gafferongames.com/post/deterministic_lockstep/)
- [Gaffer On Games — Fix Your Timestep!](https://gafferongames.com/post/fix_your_timestep/)
- [Factorio FFF #215 — Multithreading issues](https://factorio.com/blog/post/fff-215) · [FFF
  #151](https://www.factorio.com/blog/post/fff-151) · [FFF
  #415](https://factorio.com/blog/post/fff-415) · [FFF
  #421](https://www.factorio.com/blog/post/fff-421)
- [jdxdev — Deterministic prototyping in Unity
  DOTS](https://www.jdxdev.com/blog/2019/08/02/deterministic-unity-dots/)
- [Photon Quantum — deterministic ECS, sim/view
  split](https://doc.photonengine.com/quantum/current/quantum-intro)
- [SnapNet — Netcode Architectures Part 1:
  Lockstep](https://www.snapnet.dev/blog/netcode-architectures-part-1-lockstep/)
- [Box2D FAQ — determinism (cross-platform since 3.1, thread-count
  independent)](https://box2d.org/documentation/md_faq.html)
- [fpm fixed-point library](https://github.com/MikeLankamp/fpm) · [performance
  comparison](https://mikelankamp.github.io/fpm/performance.html) ·
  [Fixed64](https://github.com/SkynetNext/Fixed64)
- [google/highway — portable SIMD](https://github.com/google/highway) · [std::simd
  comparison](https://github.com/google/highway/blob/master/g3doc/std_simd_comparison.md)
- [NVIDIA — Controlling floating-point determinism in
  CCCL](https://developer.nvidia.com/blog/controlling-floating-point-determinism-in-nvidia-cccl/)
- [flecs — Systems & pipeline docs](https://www.flecs.dev/flecs/md_docs_2Systems.html)
- [The Essence of Entity Component System (arXiv, archetype vs
  sparse-set)](https://arxiv.org/html/2606.14919)
