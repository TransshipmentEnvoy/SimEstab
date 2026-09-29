# Engine Core Design

This document designs the SimEstab engine core. The core is a **deterministic simulation
kernel** that uses only integer and fixed-point arithmetic. Around it sit **peripheral
systems** that may use floats: rendering, data viz, AI and GPU compute. Peripherals affect
the core only by submitting commands. Both sides must be able to run in parallel.

Status: designed, not implemented. The built modules are `log`, `gpu`, `viz`, `util` and
`limits`. The `limits` module holds the decided values of `design_limits.md`, so the numbers
quoted here are real. Nothing in §3 to §6 exists yet.

Sections §3.2, §3.3, §3.4, §3.5, §5.1 and §5.2 are **normative**: an implementation may be
faster, but must not order operations differently.

Scope of v1. In v1, simplicity comes before progress guarantees:

- Views are `PRIVATE` only: one reader each. The multi-reader `SHARED` mode is designed but
  deferred to Appendix A until process mod hosts need snapshots; they are its only real
  user. It costs a full copy per reader, which dominates at the row counts
  `design_limits.md` §5 budgets for.
- A View filters rows (§3.4) as well as columns. Filtering rows is what makes 10⁷ live rows
  affordable; choosing columns alone cannot.
- The reader sends a small return header back to the publisher on every take (§3.5).
- Publishing may allocate memory. It never waits (§3.1).

Whether the mechanism register's progress terms (§1.1), the gate's `seq_cst` arming (§3.3)
and the endpoint lease (§5.1) can be simplified the same way is open
([Q15](open_question.md#q15-simplify-the-gate-and-the-endpoint-lease)).

Terms are defined in [glossary.md](glossary.md). Related designs: `design_python_api.md`
(Python lifecycle, options, main loop), `design_modding.md` (mod tiers) and
`design_data_container.md` (the world container, and snapshots in its §5.1).

---

## 1. The two-domain architecture

The engine has two domains: the deterministic core and the peripherals. This split is a
proven pattern. Photon Quantum calls it *Simulation vs View*, and RTS lockstep engines have
used it since Age of Empires.

```
                     commands (quantized)
   ┌────────────────────────────────────────────────┐
   │                                                ▼
┌──┴───────────────┐   snapshots / events   ┌──────────────────┐
│  Peripheral      │◀───────────────────────│  Deterministic   │
│  (float domain)  │    (one-way, copied)   │  core            │
│  render, viz,    │                        │  int/fixed-point │
│  AI, GPU compute │                        │  tick-based      │
└──────────────────┘                        └──────────────────┘
```

Two hard rules make the design work:

1. **Inbound: the only way to change core state is to submit a command** through the
   source's endpoint. Player input, AI decisions, network peers, scripts and debug tools all
   use the same path. Submission can fail, because the ring is bounded (§5.1). A command
   that was not admitted never enters the input stream, and the submitter learns this from
   the call's result, not from an event.

   The converse also holds: a command is only something a tick consumes to change core
   state. Anything that tunes the engine without touching core state (log level, pause, time
   scale) is a **control call**. It is never a command and never recorded
   (`design_python_api.md` §3).
2. **Outbound: the core publishes immutable snapshots, and events, that peripherals read.**
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
  mode. Whether it can delay ticks depends on what it is. An observer cannot (§3.3). A
  participant can, because the core waits for it. A peripheral that submits commands is a
  participant. One that only reads is an observer, unless it registers as a participant
  explicitly (§3.3).

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
  lock-free without being wait-free. (Reading a `SHARED` View would be, in Appendix A.)
- **Bounded wait**: the core may block on another party. An unbounded or undeclared wait is
  forbidden. Every wait follows three rules. It is the gate (§3.3) or one of the shutdown
  and revocation drains below. It is bounded by a declared deadline. Its expiry has a
  declared outcome.
  - The main case is **pacing**. Under lockstep, every instance advances at the rate of the
    slowest one, because every peer must run the same command set (`design_multiplayer.md`
    §3.1). Refusing to wait there would cause a desync, not a smoother tick rate.
  - The core waits for a peripheral only because it is registered as a participant, never
    because of the artifacts it uses. Nothing makes the core wait implicitly.

**One idiom recurs: publication.** Fill the payload with ordinary writes. Then make it
reachable with a single release store or read-modify-write. The consumer's acquire on that
same object makes the payload visible. Never publish by setting a "ready" flag next to data
a consumer may already be reading: that is two operations, and two operations are not a
linearization point.

**No publish path reclaims memory.** No View uses hazard pointers, payload reference counts
or deferred reclamation. GCC 16 provides neither `<hazard_pointer>` nor `<rcu>`, so any such
scheme would have to be written here. Instead, View ownership moves by a single exchange,
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
| **View** | sim → one reader; the reader's return header travels back on the same edge | publish: the release exchange of the View's control word (§3.2 (P2)); take: the release exchange at (T2), which also publishes the return header | publisher wait-free (1 exchange); reader wait-free (1 exchange) | §3.2, §3.5 |
| **Command ring** | one producer → sim | submit: the release store of the ring's write index; drain: the acquire load of it | drain wait-free (1 load); submit wait-free unless the ring is full, where it is a **bounded wait** of at most one tick | §5.1 |
| **Command outcome** | sim → producer | the outcome's release publication at the end of the tick that consumed the command | reader **blocks, bounded** by the named tick running; refused outright if the reader still holds that tick | §5.1 |
| **Endpoint lease** | producer ↔ revoker | successful lease: the second acquire load of `admission`; revocation: the `OPEN → REVOKING` exchange | submit side wait-free; revoker blocks under the shutdown deadline | §5.1 |
| **Event ring** | sim → owner thread | enqueue: the release store of the ring's write index | wait-free | §5.2 |
| **The gate** | participants and host controls → sim | a participant's `ready_through.store(t, seq_cst)`, or a `seq_cst` update of a host-control field; the sim's `seq_cst` loads. `gate_waiting` is armed before the sim re-reads, so a signal goes only to a sleeping sim | **the sim blocks, bounded** by each blocker's declared policy | §3.3 |
| **Operation lease** | any-thread API caller ↔ owner's `close()` | successful lease: the second acquire load of the access state; close: the `OPEN → CLOSING` CAS | call side wait-free; close drains under the shutdown deadline | `design_python_api.md` §2, §6 |
| **Async error handoff** | sim → owner thread | `error_state.store(READY, release)` after the winning producer fills the slot | wait-free; read at the next rendezvous | `design_python_api.md` §8 |
| **Staged join** | owner thread → sim | `stop_requested.store(1, seq_cst)`, then signal the gate if the sim is parked (§3.3); `std::thread::join` | owner blocks under a deadline | `design_python_api.md` §4.3 |

That is nine mechanisms, and most of their steady-state work is a few atomic operations.
Three contain a declared wait, and all three are the same kind of wait. The core waits for a
participant; a producer waits for ring space; and a producer waits for a tick it has already
released. How an event drain wakes a sim parked on the event backlog has no row yet
([Q11](open_question.md#q11-how-does-draining-events-wake-the-sim)).

**Any new cross-thread mechanism gets a row here, with all five items, in the same change
that introduces it.** This mirrors `design_python_api.md` §9, where every new raise site
gets a named error type.

## 2. Determinism requirements for the core

The target is **bit-identical state** from the same seed and command stream: across runs,
thread counts and build types, and later across platforms. The cross-platform part depends
on the arithmetic and checksum rules of §2.1 and §2.3. As Gaffer On Games puts it: same
initial conditions plus same inputs give byte-for-byte identical checksums, not "close"
ones.

Two different properties are involved:

- **Transition determinism** (promised): the same initial conditions and the same canonical
  command stream produce identical core checksums. The core guarantees this, and replay
  checks it.
- **Session reproducibility** (not promised): re-running a live session's external producers
  (Python timing, garbage collection, mods, GPU and AI peripherals) gives the same command
  stream. It may not, because live timing changes when commands are submitted, which tick
  drains them, and which snapshot a mod saw. The record captures what actually happened.

For lockstep multiplayer, one authority must compute and broadcast the commands of
non-deterministic peripherals. If every peer generated its own, the peers would diverge
(§6). This applies only to non-deterministic peripherals. Player input needs only a submit
that names its tick, and the gate (§3.3; `design_multiplayer.md` §4).

### 2.1 Numbers: fixed-point on integers

The core uses fixed-point numbers stored in integers. Floats can be made deterministic
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
- Trigonometric and exponential functions use fixed-point CORDIC or lookup tables. The core
  never calls libm.

### 2.2 The usual desync sources (checklist)

These come from Factorio's Friday Facts, Gaffer On Games, and lockstep postmortems:

- **Iteration order**: never iterate hash maps or pointer-keyed containers to produce state
  changes. Order must come from stable ids, not addresses or insertion timing. Every storage
  policy keeps tables dense with no free list, and where a row moves is a pure function of
  the tick's erase mask (`design_data_container.md` §2.2; §4.1).
- **RNG**: a seeded PRNG owned by the core, such as PCG or xoshiro. Each system gets its own
  stream derived from the master seed, so systems cannot disturb each other's sequences.
  Peripherals never call core generators. The generator and how streams are derived are open
  ([Q26](open_question.md#q26-the-prng)).
- **Time**: the tick counter is the clock. The core uses no wall-clock time and no float
  `dt`. The counter's width and wrap rule are open
  ([Q28](open_question.md#q28-tick-counter-width-and-wrap)).
- **Uninitialized memory and padding**: zero-initialize state. Beware of hashing structs
  that contain padding.
- **Third-party code**: any library that touches core state must itself be deterministic.
  (ODE, for example, randomized its constraint order internally.) This is the main audit
  cost of an off-the-shelf ECS in the hot path.
- **Thread scheduling**: results must not depend on thread count or OS scheduling (§4).

### 2.3 Verification from day one

Determinism is checked from the start, before there is any simulation to test. This is the
natural companion of the "session management" work in M1.

- **Per-tick checksum** of core state. Per-system checksums make it possible to find which
  system desynced; the Unity DOTS lockstep write-up found them indispensable. Whether they
  are required is open ([Q24](open_question.md#q24-are-per-system-checksums-required)).

  The checksum input is canonical, never raw memory: live semantic values in fixed
  little-endian byte order, without padding, spare capacity or SIMD tail lanes
  (`design_data_container.md` §3). Each row's id is mixed into its contribution, so the
  reduction does not depend on row order and is the same for every storage policy (§4.2,
  `design_limits.md` §6). Each object type's monotonic id counter affects the future, so it
  is part of the input too. Exactly which state must be hashed, and the bit-level algorithm,
  are open ([Q27](open_question.md#q27-the-checksums-exact-algorithm-and-inputs)).
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
  - the declared caps of each object type (`design_data_container.md` §2.2). A create at a
    cap is a deterministic rejection, so a different cap changes what the simulation does,
    and the header must catch it. An uncapped type adds nothing, because unbounded growth is
    not observable: every machine grows identically or fails;
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
  load and register core mods (including `register_column`, `design_data_container.md`
  §7.3), register mod hosts, load a replay artifact, and control calls (§1). Not allowed:
  ticks, command submission, View reads and checksums. Each of those is defined in terms of
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
2. **Close the declared caps and allocate initial storage.** Caps come from the optional
   `[[=capacity]]` annotation and from `EngineConfig.capacity`. After this step neither can
   change (`design_data_container.md` §2.2). Uncapped types get an initial allocation and
   grow from there, so this step fixes rules, not sizes. It comes after step 1 because the
   row width is not final until dynamic columns are closed.
3. **Assign source ids** and build the map between names and ids. Ids are `u32`: the host is
   0, and mods are numbered in their deterministic load order (`design_modding.md` §3). Ids
   are assigned here and bound to an endpoint when one is created. That split keeps them
   independent of whether a Tier 2 mod host ever starts. A replay starts none
   (`design_python_api.md` §4.2) and still needs the map to attribute the recorded stream,
   and a host that fails to start renumbers nobody.

   3a. **Close the View and participant registries** (§3.1, §3.3). Each View's declarations
   are now final: its projection spec, its row predicate kind (§3.4), its cadence, and
   whether it is paced. Whether a View may leave out the id column is open
   ([Q5](open_question.md#q5-is-the-id-column-in-every-projection)). The participant set,
   and each participant's deadline and expiry policy, are final too. Both registries close
   here for the same reason: they set the per-tick cost. Publish cost is a sum over Views,
   and latency is the largest deadline over participants. A cost that can change during a
   session cannot be planned for. The predicate *kind* closes here for the same reason, but
   its *parameters* do not. Parameters change how many rows match without changing the shape
   of the cost, so they arrive with each publish through the return header (§3.5). There is
   no View mode or placement to close: every v1 View is `PRIVATE` and heap-allocated, and
   both return with the `SHARED` mode (Appendix A). The host is registered as a participant
   in this step; in the simplest session it is the only one. This step comes after step 3
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
7. **Compute the phase cut** from the now-closed system list (§4.1). Then **call each core
   mod's session-start entry point**, in load order. This is the first point at which
   `resolve_column` returns a stable id (`design_data_container.md` §7.3). It is the last
   step that may fail. It runs before any side effect because it is the one step that runs
   third-party code. A mod that cannot resolve what it registered must abort a session
   nobody has seen yet, not one already publishing snapshots under a named log file.
8. **Open the per-session log sink**, named from the identity that now exists
   (`design_logging.md` §2).
9. **Publish tick 0 into every registered View** and emit `session.started`. This marks the
   start, before tick 1. The owner thread publishes it, because no sim thread exists yet
   (`run_sim_async` is not allowed in `configuring`). It is the only publish not made by the
   tick loop, and §3.2 needs no special case for it. Reading a View is not allowed in
   `configuring`, so no reader can see a View that has never been published. That state is
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

The core runs in fixed ticks and the renderer shows the latest snapshot. This is the
standard *Fix Your Timestep* structure (Gaffer On Games), without interpolation.

- **The core advances in fixed ticks**, 30 per second by default (`design_limits.md` §1).
  Real time accumulates, and the loop runs whole ticks from it.
- **The renderer runs at its own rate and draws the latest published snapshot as it is.**
  There is **no interpolation between snapshots**. World motion updates at the tick rate,
  which suits a grid-based, slow-moving establishment sim. In exchange, the renderer holds
  one snapshot instead of two, there is no interpolation factor to pass around, and
  interpolation's extra tick of visual latency is gone. Camera and UI motion stay smooth at
  the frame rate, because they are float-domain peripheral state, not snapshot data. The
  snapshot format itself does not change, so interpolation could be added later inside the
  viz layer alone if a high frame-to-tick ratio made stepping visible.

### 3.1 Snapshot publication: the View

**A snapshot reaches a peripheral through a View.** It is the only way state leaves the
core: one mechanism and one data structure.

A View is **three payload blocks plus one published word**. The core writes only a block it
owns exclusively, then publishes it with one atomic exchange. Nothing on the publish path is
reference-counted or reclaimed.

**A View is `PRIVATE`**: it has exactly one reader, and that reader is trusted.

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
View (see the rules below, and `design_modding.md` §4.3). `SHARED` returns when process
hosts do.

**A View is registered before the freeze, not requested on the fly.** The freeze closes the
registry (§2.4 step 3a). Each View declares:

| Declared | Why it is declared rather than inferred |
|---|---|
| **projection spec**: which columns | The projection is what the payload is. Declaring the columns per reader makes publish cost knowable at construction |
| **row predicate kind**: which rows | This decides whether 10⁷ live rows are affordable at all (§3.4). The kind is fixed here so the cost model is closed; its parameters change per publish through the return header (§3.5) |
| **cadence**: publish every `k` ticks | An analytics View at `k = 30` costs a thirtieth of a render View. Cadence is per View; there is no global cadence setting |
| **paced**, with a deadline | A reader that must see every tick registers a paced View and becomes a participant (§3.3) |

Whether a View may leave out the id column is open
([Q5](open_question.md#q5-is-the-id-column-in-every-projection)). How a paced View's take
counts as ready is not yet specified
([Q14](open_question.md#q14-how-does-a-paced-view-count-as-ready)).

**Placement**, heap or shared memory, is not a v1 declaration. Every View is heap-allocated
engine memory. Shared-memory placement comes back with `SHARED` and its split-permission
interprocess ABI (Appendix A, `design_modding.md` §4.3).

Three rules follow from this:

- **A process-host mod gets no View in v1.** A `PRIVATE` publisher uses the index the reader
  wrote (§3.2), so a hostile reader could steer a payload address. That cannot be handed to
  a sandbox. `SHARED` can face an untrusted reader, because the reader writes only
  block-state words that the publisher reads as availability and never dereferences
  (Appendix A, `design_modding.md` §4.3). A thread-host mod is trusted and registers
  normally.
- **The engine holds one default View.** It is `PRIVATE` and owned by the host thread. It
  projects every `[[=viz]]` column at cadence 1 with the `ALL` predicate.
  `engine.snapshot()` copies from it (`design_python_api.md` §7.2), and a reader with no
  declared needs gets it. Nobody has to register anything to see the world. `snapshot()`
  copies instead of retaining, so the owner's block is free again before the call returns,
  and the default View needs no retention rule. Its cost at large world sizes is open
  ([Q4](open_question.md#q4-can-the-default-view-afford-to-publish-everything-every-tick)),
  and so is calling `snapshot()` from other threads
  ([Q3](open_question.md#q3-can-enginesnapshot-be-called-from-any-thread)).
- **The number of Views is capped** (`design_limits.md` §2.3), because publish cost is a sum
  over them. The cap bounds the number of terms in that sum. Projection width and matched
  rows are measured, not capped.

**Publish (sim side)**, once per tick, after the terminal commit:

- For each View due this tick by its cadence, the sim does four things. It reads the
  reader's return header (§3.5) and evaluates the row predicate to a mask. It gathers the
  declared columns of the matched rows into the writable block. Then it publishes the block
  with one exchange. §3.4 covers the predicate and §3.2 the mechanism. Whether `step()`
  publishes Views that are not due is open
  ([Q12](open_question.md#q12-does-step-publish-every-view-regardless-of-cadence)).
- **Publishing never waits.** It may allocate: a block too small for this tick's matched
  rows is grown before it is filled (see growth below).
- **A `PRIVATE` View always has a writable block**, so a due publish always happens and is
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
- If the sim is faster than the reader, unread publishes are overwritten: a View is sampled,
  not consumed. If the sim is slower, the reader keeps what it has. Neither shows up as a
  failure. That is why the return header carries `last_consumed_tick` (§3.5): it is the only
  way to measure a lagging reader.
- There is deliberately no "one frame per tick" coupling through a condition variable or
  semaphore. A reader that must receive every tick registers a paced View, becomes a
  participant (§3.3), and pays for that guarantee at the gate. A reader that only wants to
  shed load sends a cadence hint (§3.5) and stays out of the gate.

**CPU/GPU lifetime boundary: the GPU never reads View memory.** `render()` copies the
`[[=gpu]]` columns on the CPU into a **staging transfer buffer**, then unmaps it. Under the
SDL3 backend the staging buffer is mapped and unmapped every frame, because SDL does not
allow upload commands to be recorded while a transfer buffer is mapped. Destination buffers
are cycled and rewritten whole, one per column. Each keeps a `last_uploaded_snapshot_tick`,
advanced when its fence signals and used only to skip an upload when the tick has not
advanced. Only uploads whose fence has completed are presented. The full contract is in
`design_data_container.md` §5.

**Growth.** Tables have no size limit by default (`design_data_container.md` §2.2), so a
growing world grows its projections, and so does a predicate that matches more rows than
last tick. Growth happens after the exchange and before the fill. At that point the
publisher's block is its own, so it may be reallocated in place.

**No block is freed during a session.** Allocation is tick-budget work done before the
release publication; it is never part of the bounded atomic step. Blocks come from a pool
that never returns pages to the OS. A freshly mapped block costs one page fault per page on
first fill, so reusing a warm block is what keeps publish time steady. A pool that freed and
remapped would make that jitter worse.

**The pool may also grow in number of blocks.** v1 never needs this, because a `PRIVATE`
View always has a writable block. Under `SHARED`, the alternative to growing the pool is
skipping a publish (Appendix A), and growing is the better answer. Either way, the atomic
step itself never allocates. Block growth has no line in the tick budget yet
([Q20](open_question.md#q20-where-does-view-block-growth-go-in-the-tick-budget)).

**Cost, and where it is charged.** Per-View projection costs more than one shared projection
would, in one way:

- Publish cost per due View is `scan(predicate inputs × live rows) + gather(matched rows ×
  spec)`, not one full copy. Two Views over the same columns cost twice. That is the price
  of keeping blocks without copying, for both.
- **The row predicate shrinks the gather term.** A render View over 10⁷ live rows whose
  `FRUSTUM` matches 10⁴ gathers 10⁴ rows. Without a predicate, the same View is 320 MB per
  publish at a 32-byte spec (`design_limits.md` §5), and no choice of columns makes that
  affordable. That is why the row filter is a declaration and not an optimization.
- **The scan term remains, and it only reads.** A spatial predicate reads its input columns
  for every live row, sequentially and without writing: about 12 bytes per row for a
  position test. (Whether 12 bytes is right for Q32.32 coordinates is open:
  [Q61](open_question.md#q61-what-row-width-does-the-predicate-scan-assume).) This is the
  real floor of the design. The two ways below it are a spatial index behind the predicate
  kind (§3.4), and running scan and gather on the worker pool. A narrower spec does not
  help.
- The projection spec itself is the other lever. A render View takes the `[[=viz]]` columns,
  a GUI View takes two, an analytics View takes five at `k = 30`. With declared column
  lists, total bytes are normally below one undeclared full projection. That is why the
  column list is a declaration and not a default.
- **There is no maximum width per row** (`design_limits.md` §5). A per-row cap would bound
  the wrong thing: cost is width times matched rows, and neither is capped.
- **Cost is measured, not capped, and crossing the threshold is diagnosed.** Publish
  duration, publish bytes per second and matched rows are first-class metrics, per View and
  in total. **The threshold is 4 GB/s, checked per View, not against the total**
  (`design_limits.md` §5). It is a bandwidth, so it does not depend on tick rate or cadence.
  It is per View because every remedy here (narrow the spec, tighten the predicate, lower
  the cadence) changes one View's declaration, so a warning must name one View. Crossing it
  logs a warning naming the measured rate and the View. It never rejects anything, and the
  simulation does not change. The total is still reported, but the threshold is not checked
  against it.
- If measurements exceed the budget, the safe offload options, in order of preference:
  1. scan and gather on the worker pool. This is safe because the world does not change
     between the terminal commit and the next tick, and order is kept because the gather
     positions come from a prefix sum (§3.4);
  2. a bit-exact copy of core data on the sim thread, then fixed-to-float conversion on
     another thread.

  Until profiling justifies one of these, the publisher does the projection. Publishing only
  changed data (partial or dirty publish) is ruled out (`design_data_container.md` §5); the
  row predicate is a different axis and does not change that.

**Metrics.** Per View: publish duration, publish bytes per second, matched rows, last
published tick, and **consumer lag**: last published tick minus `last_consumed_tick`, read
from the return header (§3.5). Without lag, a `PRIVATE` reader that falls behind would be
invisible, because the publisher simply overwrites and nothing fails. Sustained lag
identifies a slow reader.

**Commands per tick is a first-class metric too**, in total and per source. A tick runs
everything stamped for it (§5.1), so nothing below `C` limits the drain cost. It is whatever
the session's producers submit. A session approaching that ceiling should show as a rising
number long before it shows as a frame-time problem. The per-source figure names the
endpoint to narrow, just as the per-View figure does for publish.

**The tick budget is the wall-clock length of one tick**: 33.3 ms at 30 Hz
(`design_limits.md` §1). These costs are charged against it:

| Charge | Shape | Bounded by |
|---|---|---|
| Publish | `Σ over due Views (scan over live rows + gather over matched rows × spec)` | **nothing, by design**. Measured, and each term is checked against 4 GB/s **for that View** (above; `design_limits.md` §5). The View cap bounds the number of terms; each term's gather is bounded by its row predicate (§3.4); its scan stays proportional to live rows |
| Compaction scan and gather | O(rows × columns) per object type that erased this tick | **nothing, by design**. Measured and checked against its own `compaction_warn_bytes_per_second`, per object type and in total (`design_limits.md` §4). For a capped type, rows ≤ cap (`design_data_container.md` §2.1). The number of types and the column width are not capped; a mod's share of the width is attributed to that mod, not refused |
| Growth: reallocate and copy | O(rows × columns) on a tick that doubles a table, per growing object type; **done together with compaction** when the same commit does both | geometric growth, so O(1) per create on average. But it comes in bursts, and a burst has no bound for an uncapped type |
| Upload bytes per frame | the render View's spec over its matched rows, whole columns | same treatment as publish |
| Command drain and execution | O(commands stamped for this tick) | `C = Σ capacity(endpoint)` over the registered endpoints (§5.1, `design_limits.md` §2). Only a session that registers that many endpoints, each at full capacity, reaches it |
| System execution | the actual simulation work | not yet measured |

**Time waiting at the gate is not charged.** Time at the gate (§3.3) is pacing, not work:
the core is deliberately not running. Counting it would make a healthy lockstep session look
like an overrun.

### 3.2 View protocol (normative)

§3.1 fixes the design. This section fixes the mechanism, with the five items of §1.1. It is
normative: an implementation may be faster, but must not order operations differently.

**State.** The one atomic field is naturally aligned and always lock-free on the supported
x86-64 baseline: the `PRIVATE` word is 4 bytes. No 16-byte atomic and no tagged pointer is
allowed.

```
struct PrivateView {
    Block*            block[3];
    std::atomic<u32>  word;
    Return            ret[3];        // §3.5; consumer-written, parallel to block[]
    u32               w;             // publisher-private
    u32               r;             // reader-private
};
```

The private indices are plain variables, each touched only by its owner. `ret[]` is plain
memory on both sides. It is ordered by the same exchange that transfers the block, never by
an atomic of its own (§3.5).

`word` is `(index << 1) | dirty`.

```
publish:  fill *block[w]                                    # (P1) plain; the block is private
          prev = word.exchange((w << 1) | 1, acq_rel)       # (P2) LINEARIZATION POINT
          w    = prev >> 1

take:     if ((word.load(relaxed) & 1) == 0: return HELD    # (T1) nothing new
          prev = word.exchange(r << 1, acq_rel)             # (T2) LINEARIZATION POINT
          r    = prev >> 1
```

**Theorem.** `{w, r, word >> 1}` is a permutation of `{0, 1, 2}` at every instant. So `w !=
r` always, and the publisher never writes the block the reader holds.

*Proof:* each operation is one exchange that swaps a private index with the shared one,
which is a transposition. The initial assignment is a permutation, and transpositions keep
it one. No interleaving needs checking, because each private index is written only by its
own side, and only after its own exchange returns.

The ordering works in both directions. The acquire half of (T2) sees the publisher's
released payload. The release half of (T2) pairs with the acquire half of the next (P2), so
all the reader's loads happen before the publisher reuses the returned block.

**The return header rides that reverse edge (§3.5).** The release half of (T2) already
publishes every earlier write by the reader, including its writes to `ret[r]`. `ret[r]` is
an ordinary object and needs no atomic of its own. The return channel is therefore a payload
on a synchronization this protocol already performs and already tests, not a second
mechanism.

**Memory orders.** (P2) and (T2) are `acq_rel`, for both directions of ownership. That one
pair carries the payload forward and the return header back. Nothing else in the protocol is
atomic.

**Progress.** Publish and take are wait-free, in one exchange each.

**Failure and deadline behaviour.** The only failure is the local API refusal while array
views of the previous block are alive (§3.1). A due publish cannot be skipped, and a take
cannot fail for any reason outside the caller.

**`hardware_destructive_interference_size` depends on build flags.** It can be overridden
with `--param` and follows `-mtune`, and GCC 16 does not warn when it is used in an
ABI-visible position. **A heap-allocated View is deliberately not ABI-stable**: it is engine
memory, never serialized, and its layout never crosses a module boundary. The deferred
shared-memory View is the opposite case, and fixes every field width and offset as literals
(`design_modding.md` §4.3).

**Verification.** A functional test cannot tell a correct implementation from a broken one
on x86-64. The sanitizer test checks:

- the permutation invariant;
- the reverse happens-before edge from reader to publisher;
- riding that same edge, that a return header written before a take is read intact by the
  publisher that receives the block, and never while the reader is still writing it (§3.5).

A race between growing or replacing a block and reusing it is a required test case.

### 3.3 Participants and the gate (normative)

An observer cannot delay a tick (§3.1). This section says what can. The gate is the only
place in the engine where the core waits.

**Every peripheral is one of exactly two kinds.** Which kind follows from what it does, not
from a free choice at registration:

| | **Participant** | **Observer** |
|---|---|---|
| The core… | waits for it, up to its declared deadline | never waits for it |
| Reads | its View | its View |
| Writes | commands | **nothing**; reading the world is not participation |
| Costs | up to its deadline of tick latency | a projection copy |
| Examples | the host, network peers, every mod that submits commands, the replay harness, a recorder that must not miss a tick | viz and GUI, analytics, an agent mind that only watches |

**Submitting makes a peripheral a participant; reading does not.** A command names the tick
it applies to (§5.1), and a tick cannot run until every source that acts in it has finished
submitting. So every producer is paced, and **no producer anywhere in the engine is
unpaced.** Reading gives no such standing: an observer may fall any distance behind, and the
core never notices. One case is declared rather than implied: a peripheral that submits
nothing but must not miss a tick, such as a recorder or a training-data collector. It
registers as a participant explicitly, with a paced View (§3.1). Whether mods that never
submit must still be paced every tick is open
([Q2](open_question.md#q2-must-every-mod-be-paced-every-tick)).

Tier 3 core mods are neither kind. They are systems inside the tick, ordered by the phase
cut (§4.1), and this section does not apply to them.

**Pacing gives acknowledgement, not access.** A participant reads exactly what an observer
reads: its own View. Being paced guarantees that the core will not run ahead of what the
participant has acknowledged. It gives no view of live core state. **No peripheral of any
kind ever reads live core state.** That keeps §1's one-way boundary intact while allowing
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
    std::mutex       transition;       // pause/resume/step compound transitions only
    bool             step_in_flight;   // guarded by transition; never read by sim
};
Participant                participants[N_PARTICIPANTS];   // fixed at the freeze
HostControl                host;
std::counting_semaphore<>  gate;                           // blocking + deadline ONLY
std::atomic<u32>           gate_waiting;                   // 1 only while the sim is parked
std::atomic<u64>           first_unexecuted;               // sim-written; the tick ledger
std::atomic<u32>           tick_epoch;                     // 4 bytes; sim -> owner wakeups
u64                        backlog;                        // §5.2; PLAIN, sim-thread-only
```

Each cross-thread host field is its own naturally aligned atomic, on purpose. A stop, a
pause and a run grant are independent facts, so storing one must never erase another. The
mutex serializes control callers on the cold path only. The sim thread never takes it, so
the gate's hot path stays plain loads.

**The event backlog is not a host field.** `backlog` is a plain `u64` owned by the sim
thread alone. The gate reads it like any other local variable. It is not an atomic, and in
particular not a flag, for the reason §5.2 gives.

**The loop.** This is the engine's whole tick loop. The core's only wait on another party is
the one marked:

```
for (;;) {
    for (;;) {
        gate_waiting.store(1, seq_cst);                   # (G1) ARM, then re-read
        if ((blocker = blocker_for(t)) == NONE) break;    # (G2) predicate reads: seq_cst
        if (blocker == STOP) { gate_waiting.store(0, relaxed); return; }
        if (!gate.try_acquire_until(absolute_deadline(blocker, t)))
            on_gate_timeout(blocker, t);                  # THE ONE WAIT
    }
    gate_waiting.store(0, relaxed);
    drain_commands(t);                                    # §5.1
    execute(t);                                           # §4.1 phases + commits
    publish(t);                                           # §3.2, per due View
    first_unexecuted.store(t + 1, release);
    tick_epoch.fetch_add(1, release); tick_epoch.notify_all();
    ++t;
}

blocker_for(t), in priority order:
    if host.stop_requested.load(seq_cst): STOP
    if backlog >= high_water: HOST_BACKLOG               # plain local read (§5.2)
    if host.run_until.load(seq_cst) <= t: HOST_PAUSE
    first active p with p.ready_through.load(seq_cst) < t: p
    otherwise: NONE
```

A participant becomes ready with two operations and no other coordination:

```
ready_through.store(t, seq_cst);                   # (G3) LINEARIZATION POINT
if (gate_waiting.load(seq_cst)) gate.release();    # (G4) wakeup, and only if one is needed
```

**Signal only a sleeping sim.** The condition in (G4) is required, not an optimization.
Every participant becomes ready once per tick, and a healthy session never blocks. If
participants signalled every time, one unused credit per participant per tick would pile up
in the semaphore for hours. The first real wait would then spin through all of them at full
CPU, re-checking a predicate that is false each time, before it finally blocked. The
semaphore's count is not the condition (below), so the extra credits would waste time rather
than corrupt state. But the one wait in the engine must not burn a core for as long as the
session has been healthy.

**Why both sides use `seq_cst`, and nothing else in the engine does.** (G1) with (G2), and
(G3) with (G4), are each a store followed by a load, on opposite sides of the same two
objects. Acquire/release cannot order that pattern. Without one total order over the four
operations, the sim could read an old `ready_through` while the participant reads an old
`gate_waiting`, and the wake-up would be lost for the whole deadline. Under `seq_cst` the
four operations fall into one total order, so at least one side sees the other: either (G2)
sees the readiness and does not sleep, or (G4) sees the arm and signals.

This costs one fenced store per participant per tick, and one per tick on the sim. It
replaces one semaphore `release()` per participant per tick, which is a system-call-class
operation, so arming is cheaper than what it replaces even before counting the pile-up.
Every control write that can change what `blocker_for` returns, namely `run_until` and
`stop_requested`, arms the same way: store `seq_cst`, then signal only if `gate_waiting`
reads 1. The same rule is reused wherever one side parks on another's store (the admission
wait, §5.1). How an event drain wakes a sim parked on `HOST_BACKLOG` is open
([Q11](open_question.md#q11-how-does-draining-events-wake-the-sim)). A lost wake-up is a
hang, not a data race, so how to test these handshakes is also open
([Q6](open_question.md#q6-how-are-the-wake-up-handshakes-tested)).

**Why a semaphore, and why its count is not the condition.** C++26 has no timed atomic wait:
`<atomic>` has no `wait_for` or `wait_until`, and neither do `latch` or `barrier`. So
`std::counting_semaphore::try_acquire_until` is the only standard primitive that gives both
a happens-before edge and a deadline. It is used only for blocking and the deadline. The
condition is always re-evaluated from `ready_through`, never inferred from the semaphore's
count. A stray or duplicated `release()` then costs one extra loop iteration that finds the
predicate already satisfied. If the count were the condition, an abandoned wake-up could be
consumed by an unrelated later wait and report progress that never happened.

Arming makes stray credits rare, since a release needs an observed `gate_waiting == 1`, but
not impossible: a signaller may see the arm just as the sim leaves (G2). One leftover credit
costing one extra iteration is acceptable. What arming rules out is credits piling up
without bound. `ready_through` only ever increases for the same reason: `atomic::wait` and
any flag-style condition are defined against transient values and can miss a condition that
is true only briefly.

**Deadlines are absolute and share one anchor.** On the first blocker for tick `t` that is
not a pause, the sim records one `wait_started[t]`. Every participant's deadline for that
tick is `wait_started[t] + participant.deadline`, even if `blocker_for` reaches that
participant later. A stray semaphore credit never recomputes either value from `now`. So
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
| a producer's admission or outcome wait (§5.1) | suspended, and the waiter is woken with `session_paused` |
| a mod's inbox-drain and `on_unload` budgets (`design_modding.md` §6) | suspended. Shutdown is not a pause, so this matters only if a pause overlaps shutdown |
| the event backlog (§5.2) | it is itself a pause, so there is no second clock to stop |

**Every waiter is woken on pause, with a reason.** Every producer is paced and every
producer waits (§5.1). A pause that left waiters asleep would park every mod thread on a
condition only a running engine can satisfy, and inside every mod the pause would look like
a hang. So a pause wakes all of them with `session_paused`, which means "the wait is still
yours to retry", never "your operation failed". Whether callers actually see
`session_paused` is open ([Q7](open_question.md#q7-does-a-caller-ever-see-session_paused)).

`HOST_PAUSE` is intentionally indefinite. Each semaphore wait uses a bounded diagnostic
slice whose expiry means `remain parked`, never `FAIL`. `HOST_BACKLOG` works the same way,
for the same reason (§5.2): an application that stops draining events pauses the simulation
and is reported, rather than timed out into a dead session.

**Progress.** The sim blocks, bounded by the smallest declared deadline among participants
that are not ready yet. Each participant publishes its readiness wait-free, in one store.
Participants wait at the same time, so the worst latency the gate adds to a tick is the
largest deadline in the blocking set, not the sum. That is what makes several participants
affordable.

**The host is always a participant.** The freeze registers it. Its predicate is a
combination of independent fields rather than one overloaded word:

| Host state | Representation |
|---|---|
| running | `run_until = U64_MAX`, `stop_requested = 0`, `backlog < high_water` |
| `pause()` | under `transition`, set `run_until = first_unexecuted` |
| `step(n)` | under `transition`, reserve `step_in_flight`, then set `run_until = first_unexecuted + n` (checked for overflow) |
| event backlog at its high-water mark (§5.2) | **not a host field at all**: `backlog` is a plain sim-local value, so a resume cannot race it and a drain cannot erase it |
| `request_stop()` | `stop_requested.store(1, seq_cst)`; sticky and highest priority |

`design_python_api.md` §4.1 describes host pacing differently, and the two are not yet
reconciled ([Q1](open_question.md#q1-how-is-the-host-paced)).

Pause, single-step, backpressure and shutdown still form one gate predicate, but no two
share storage, so concurrent controls cannot lose a stop or unblock a backed-up ring.
`pause`, `resume`, and the setup and teardown of `step` serialize on `transition`. So
`resume()`'s "no step in flight" check and its store form one transition, not a
check-then-act race. `request_stop()` needs no mutex, because nothing ever clears it. The
backlog needs none, because it has one writer.

`run_until` is an exclusive ceiling, so pausing before tick 0 can be represented without
unsigned underflow: tick `t` may run exactly when `t < run_until`.

`step(n)` records `start = first_unexecuted`, sets `run_until = start + n`, and returns when
`first_unexecuted >= start + n`, waiting on `tick_epoch`. The host's own run grant then
blocks the gate, so the sim is provably parked when the call returns. That makes
`checksum()` on the next line a well-defined read of quiet state rather than a race
(`design_python_api.md` §4.3, §6).

**Registration** happens before the freeze, which closes the set (§2.4 step 3a). It declares
the deadline and the expiry policy. There is **no global default**. Registration is where
the cost is taken on, so it is where the cost is declared, just as a mod manifest declares
its endpoint capacity. The count is capped (`design_limits.md` §2.1). The cap is generous
because a participant costs an array entry scanned once per tick, not an allocation. No
manifest or config field yet declares a mod's deadline and expiry policy
([Q9](open_question.md#q9-how-does-a-stopped-participant-leave-the-gate)).

**On expiry.** The participant set never shrinks, since the freeze fixes it. What changes is
whether a participant can still hold the gate. The `DROP` row is a correctness constraint,
not a preference:

| `on_expiry` | Behaviour | Use |
|---|---|---|
| `FAIL` | log `critical` naming the participant and tick, retry once, then the engine enters the terminal `failed` state | the host: a stuck owner thread is a defect, not a load condition |
| `CONTINUE_WITHOUT` | `active.store(0, release)`, so later readiness stores cannot re-enter the conjunction; emit a reliable-class event; keep ticking | a paced recorder or mod whose absence cannot change what the core computes |
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

`CONTINUE_WITHOUT` is allowed only for a participant whose absence provably cannot change
the command stream. That excludes a peer, and every mod that submits commands, since
submitting is what makes a peripheral a participant. `SUSPEND` is the answer for those. It
does change the command stream, so it is reported and the session sees it, but it is
reversible and never ends the session.

**When the host is the only participant, the conjunction has one term.** While the session
runs, the host's run grant is `U64_MAX`, `stop_requested` is clear and the backlog is below
its mark. So the gate costs two loads and one plain comparison per tick, and never blocks.
The gate built for multiplayer costs only this at its default setting
(`design_multiplayer.md` §6).

### 3.4 Row predicate (normative)

**Each View filters on two axes.** Columns are the projection spec (§3.1). Rows are this
section. Without row filtering, a View over 10⁷ live rows copies all of them to a reader
that wants 10⁴. No column choice fixes that: a 32-byte projection of 10⁷ rows is still 320
MB per publish (`design_limits.md` §5).

**A row predicate cannot cause a desync**, and the rest of this section relies on that. A
View is an output; core state cannot be rebuilt from a snapshot (§5). So two peers may
filter to completely different row sets and stay in lockstep. The predicate needs no
determinism, takes no part in the checksum, and never enters the command stream.

**The kind is declared; the parameters are not.** A View registers one kind from a closed
set:

| Kind | Parameters | Use |
|---|---|---|
| `ALL` | none | the default, and what every View has until measurement says otherwise |
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
columns always works, and it is the baseline charged in §3.1. If the core already keeps a
spatial structure for its own systems, `SPHERE` and `FRUSTUM` may be answered from it, and
the scan cost disappears. That is an implementation choice behind the declaration. A reader
never declares or depends on it.

**The algorithm is already specified elsewhere.** Evaluate the predicate to a mask,
exclusive-scan the mask into destination offsets, and gather each column. That is exactly
order-preserving compaction (`design_data_container.md` §2.2) run against a different mask.
Building compaction builds this. It runs in parallel on the worker pool using a prefix sum,
and keeps ascending row order without extra care. It is safe there because the world does
not change between the terminal commit and the next tick.

**Coarse in the core, exact in the reader.** The core's predicate is conservative: a region,
not a final visibility answer. The reader refines it against its own fresh camera, on the
rows that passed. This split has three benefits:

- the core never needs the exact camera, so the one-tick staleness of the parameters (§3.5)
  needs no safety margin: the region is already loose;
- refinement is always safe: filtering too little draws too much, filtering too much drops
  something the reader is responsible for, and neither can reach core state;
- the parameters stay small, so the return header can be a fixed struct rather than a
  variable-length message.

**A filtered View carries the id column; an unfiltered one need not.** Rows 3, 17 and 902
mean nothing to a reader without the `[[=id]]` column. So a View whose predicate is not
`ALL` includes `id` automatically, at 8 bytes per matched row. The render View is the
exception. The GPU may not key on a row offset across frames anyway
(`design_data_container.md` §5), so a renderer that only draws needs no id and does not pay
for one. Whether a View may leave out the id column at all, and how it declares that, is
open ([Q5](open_question.md#q5-is-the-id-column-in-every-projection)).

**Filtering is turned on by measurement.** `ALL` is the default, and it is right at 10⁵ live
rows, where a full projection is a few megabytes and a predicate would add complexity for no
gain. Row filtering is a scaling feature, turned on when `publish bytes/second` calls for
it. Caps and table growth work the same way.

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
struct Return {                      // consumer-written, publisher-read
    u32              seq;            // monotone; publisher keeps the newest it has seen
    u64              last_consumed_tick;
    u32              cadence_hint;   // 0 = no preference
    PredicateParams  params;         // §3.4; fixed size, kind-dependent interpretation
};

Return ret[3];                       // parallel to block[3]; never reallocated
```

The headers sit in a parallel array rather than inside the blocks, for two reasons. Growth
reallocates a block and would have to preserve a header stored inside it. And a header in a
block's first cache line would false-share with the publisher's fill. `ret` has a fixed size
and never grows.

**Protocol.**

```
consumer:  fill ret[r]                     # plain; r is the block it holds
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

**The return channel may shape the View; it must never reach the world.** A cadence hint
changes how often this View is published, and nothing else. A predicate parameter changes
which rows this View contains, and nothing else. Anything the core acts on goes through the
command ring (§5.1), which is the ordered, deterministic, replayed and checksummed input
path. A second input that is unordered, undeclared and invisible to replay would be a desync
source. This channel is convenient, which is exactly why the rule is stated here.

**The cadence hint is advisory.** The publisher may ignore it. A reader that must receive
every tick registers a paced View and becomes a participant (§3.3), paying for that
guarantee at the gate. The hint lets a reader shed load without blocking the core; pacing
lets it refuse to miss a tick. They meet different needs, and neither replaces the other.

**Trust.** The return header keeps `PRIVATE`'s trust boundary; it does not widen it. The
publisher already uses the block index its reader writes (§3.2), so a reader able to corrupt
the header could already do worse. For the same reason the return header does not extend to
a sandboxed reader. A `SHARED` View exposes no reader-written bytes that the publisher
dereferences (Appendix A), and giving it a return header would be a new decision, not this
one extended.

## 4. Parallelism inside the deterministic core

The core can be highly parallel. The only constraint is that **the merged result must not
depend on scheduling**.

**Units.** There are exactly two, and neither is spatial. Neither is called a "partition":
in this codebase that word means only a C++20 module partition (`design_patterns.md` §1).

| Unit | What it is | Unit of |
|---|---|---|
| **chunk** | a fixed range of row offsets within a column. Its size is defined once, as `LIBSIM_ESTAB__CHUNK_ELEMENTS`, in `design_data_container.md` §4 | SIMD iteration, false-sharing isolation, parallel-for work items |
| **task range** | the contiguous run of whole chunks given to one parallel invocation | write scoping: the `begin` and `end` of a mod's write span (`design_data_container.md` §7.3) |

Task ranges never overlap, by construction: the parallel-for hands out non-overlapping chunk
runs, so no system ever has to assert it.

Row offsets last one tick; entity ids are permanent (`design_data_container.md` §2.2).
Everything below is written in terms of row offsets, because nothing inside a phase needs an
id.

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
earlier, because the system list is not closed until every core mod has registered. It must
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
| Does | merges `write_staged` buffers, flips `write_dbl` columns | applies **all** structural changes: creates, erases (and so compaction), relationship rewiring |
| Changes row counts? | never | yes, and only here |

Both apply their staged input in a fixed order, by task index or row offset, never by the
order threads finish, to produce tick N+1. The one exception, and why, is at the end of this
subsection. Staged buffers with a fixed-order commit are required for anything that might
conflict or change structure. That covers shared claims, reductions that are not
order-independent, create and delete, relationship rewiring, and any write whose destination
is found at run time. They are not required for ordinary `write_local` column writes. Tier 3
mods sign the same contract (`design_modding.md` §5.2).

**Structural changes happen only in the terminal commit.** This has four consequences:

- **Row offsets are stable for the whole tick**, not just within a phase. That is why every
  tier can call a row offset tick-scoped (`design_data_container.md` §2.2), instead of
  "phase-scoped, so count your phases".
- **Compaction runs at most once per tick**, so its O(n) scan and gather is one line in the
  tick budget (§3.1). Applying structural changes at every boundary would repeat that cost
  at every boundary where something was erased: the same work, paid k times, for nothing.
- **Creation is deferred.** An entity a system creates in phase 1 cannot be addressed until
  the next tick, because it has no row until the terminal commit assigns one. flecs and
  Unity DOTS defer structural changes the same way, and it is the price of the first two
  points. A system that must act on a new entity in the same tick has to run after whatever
  creates it, and read the staged create buffer instead of the columns.
- **Growth happens here, and so does the only way a create can fail.** Tables have no size
  limit by default (`design_data_container.md` §2.2). The commit computes the required row
  count before moving anything, and reallocates geometrically if it exceeds the current
  allocation. It writes the compaction gather directly into the new allocation, so a tick
  that was compacting anyway pays nothing extra. Reallocation changes addresses, not row
  offsets, and every column pointer expired with its `tick_fn`, so row stability still
  holds. **Allocation failure ends the session; it is never a rejection.** A rejection
  caused by host memory would make two machines replaying the same stream accept different
  creates.

  For an object type that declares a cap, the commit accepts creates only while rows remain.
  The remaining count is taken after compaction, since erases staged this tick free their
  rows in the same pass. Staged create buffers then merge in fixed task order: the first
  that fit are accepted, and the rest are rejected and reported. Task order makes the
  accepted set a function of state, not of scheduling. Without it, a full table would turn
  the parallel phase into a desync source, which is the one thing §4 exists to prevent. Ids
  are assigned here, only to accepted creates, in the same order. What kind of cap an object
  type has, and what "stop" means for a ceiling, are open
  ([Q31](open_question.md#q31-what-does-hitting-a-declared-cap-do)).

A boundary commit is cheap by construction: it touches only the columns that systems in the
closing phase actually staged. Only the terminal commit is O(n).

**Parallelism inside a commit differs by part, and the difference matters:**

- The **staged merge is sequential**, in fixed task order. That order is the determinism
  guarantee. Running it on the worker pool would bring back exactly the dependence on
  completion order that staging exists to remove.
- The **compaction scan and gather may use the worker pool.** Every row's destination is a
  pure function of the erase mask, computed before any row moves, so the result is the same
  for any thread count: it is a deterministic permutation, not a merge. This is the one part
  of a commit allowed to run in parallel, and it is the part that costs O(n). Whether the
  parallel gather must write to a separate buffer to avoid overwriting unread rows is open
  ([Q33](open_question.md#q33-the-parallel-compaction-gather)).

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

**No structural change and no row movement happens during the tick's phases.** Row counts
are fixed from the first phase to the last, because creates, erases and compaction all
happen in the terminal commit (`design_data_container.md` §2.2). Without this, the read-span
rule above could not be proven, because a row offset could change meaning in the middle of a
tick.

The guarantee depends on what `create` at the current allocation does: **tables grow in the
terminal commit, and a cap is optional** (`design_data_container.md` §2.2). Growth
reallocates, and that is compatible with the guarantee. Row stability is about row offsets,
which a reallocation keeps. The only pointers into columns are valid for one `tick_fn` call
(`design_data_container.md` §7.3), and that call has ended before the commit runs. "No row
movement during the tick" was never a claim about addresses.

Growth does cost three things. There is a bursty `O(rows × columns)` copy on ticks that
double a table (the budget in §3.1). Payload blocks in every View are replaced (§3.1). And
world memory cannot be known at the freeze. Declaring a cap removes all three for that
object type, which is why the annotation exists.

**No spatial colouring.** Checkerboard colouring over a spatial grid, as Factorio does, is
deliberately not part of this design. Colouring only means something over a spatial unit,
and a spatial region's rows are an arbitrary scattered subset of row offsets. So colouring
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
gives neither by itself. For storage policies that keep ids sorted, the canonical-order
option is a plain linear scan, and so the cheaper of the two.

**The checksum is the id-mixed reduction, for every storage policy** (`design_limits.md`
§6). The cheaper option is not kept as a second path for the two ordered policies. It would
save a little per tick, at the cost of two algorithms, two code paths, and a class of bug in
which changing a storage policy silently changes what a golden replay compares. One
algorithm catches permutations everywhere, and it is the only one that works for `unordered`
(`design_data_container.md` §2.2), which has no canonical row order. The function is a
hand-written 64-bit mix rather than an external hash, because bit-exactness across compilers
matters most, and it vectorizes over SoA columns. Its bit-level specification belongs to §7
step 2 and is open ([Q27](open_question.md#q27-the-checksums-exact-algorithm-and-inputs)).

### 4.3 What to avoid

- **Concurrent containers** (concurrent hash maps and the like): insertion order depends on
  scheduling. The Unity DOTS lockstep project had to remove `NativeMultiHashMap.Concurrent`
  for exactly this reason.
- **Atomics that pick winners** ("the first thread to claim X wins"): the winner depends on
  timing. If contention needs resolving, collect all claims, then resolve them in a
  deterministic pass (sort by row offset).
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

Integer SIMD is fully deterministic: the same bits on SSE, AVX and NEON. So the core can use
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
  dispatch is safe here only because the core is integer-only: every instruction-set path
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

- **Core world data lives in custom SoA tables** (M3, the world container). They hold dense
  columns of fixed-point and integer components, and permanent monotonic entity ids that
  resolve to tick-scoped row offsets (`design_data_container.md` §2.2). Fixed chunking
  serves task ranges and SIMD alignment. Archetype-style dense tables iterate faster and
  vectorize better than sparse sets; sparse sets win only under heavy add/remove churn.
- **flecs is optional, for the cold path**: entity lifecycle bookkeeping, composing
  peripheral displays, editor and debug queries. These are places where determinism does not
  matter. Whether to keep flecs at all is open
  ([Q56](open_question.md#q56-keep-or-drop-flecs)).
- The core's system scheduler can then be a simple explicit list: a fixed system order, each
  system declaring an access kind per column, and phases derived once, statically, by the
  cut in §4.1. Explicit ordering worked better than attribute-driven ordering in the DOTS
  lockstep experience.

## 5. Peripheral systems (float domain)

Peripherals are everything outside the core. Every peripheral here is an observer unless it
registers as a participant (§3.3). None reads live core state, and none can delay a tick by
lagging.

- **Renderer and viz**: reads a `PRIVATE` View over the `[[=viz]]` columns at cadence 1 (no
  interpolation, §3), converts fixed-point to float once, and is free to use floats, compute
  shaders and Vulkan. Nothing flows back.
  - **Threading**: preparation and GPU compute may run on workers, but presentation is a
    backend contract, not an architectural freedom. Under SDL3, the window, swapchain
    acquisition and present belong to the owner thread (`design_python_api.md` §4.3). A raw
    Vulkan backend could present from any thread.
  - **Memory**: uploads and dispatches read only the renderer's own staging and destination
    buffers, never View memory (the CPU/GPU lifetime boundary in §3.1).
  - **Backend scope**: Vulkan only (SPIR-V from glslang), covering Linux and Windows. D3D12
    and Metal need DXIL and MSL and are future work, so macOS has no GPU path
    (`design_data_container.md` §5).
  - **Losing the GPU device ends the session.** It is logged as `critical`, the engine
    enters `failed`, and there is no recover-and-re-upload path. Core state is untouched and
    the session can still be replayed, because the GPU is in the peripheral domain. Whether
    this deserves its own terminal state is open
    ([Q17](open_question.md#q17-should-resource-failures-have-their-own-terminal-state)).
- **GUI**: a View like any other, usually a few columns at cadence 1, plus the host's
  command endpoint. It is not a special case of anything. Under lockstep, a `pause()` from
  the GUI is agreed across the session rather than applied locally, because it changes the
  run grant that feeds the gate check every peer uses (`design_multiplayer.md` §3.3).
- **Data viz and analytics**: a View at a low cadence. Five columns every thirty ticks costs
  what its declaration says.
- **AI**: reads a View (stale data is fine and realistic), thinks in floats or on the GPU,
  and emits commands. Commands are recorded in the input stream, so AI non-determinism does
  not affect the simulation. If a replay from the seed alone is ever needed, the AI must
  either be deterministic (integer inference) or its commands must count as external input;
  §6 describes the hybrid split this design uses. A planner whose reasoning spans several
  ticks must carry entity ids, not row offsets, and re-resolve them against a fresh snapshot
  before submitting. Row offsets mean something only inside the snapshot that produced them
  (`design_data_container.md` §2.2).
- **GPU compute for the core?** Peripheral-only for now. Float GPU work is non-deterministic
  in practice: atomic commit order, per-driver shader compilation and reduction scheduling
  all vary (see NVIDIA's CCCL determinism levels). Integer-only compute shaders are
  bit-exact in theory, but driver variance makes this a research project, not a foundation.
  GPU results re-enter the core only as quantized commands, like AI.

The only artifacts peripherals ever see:

| Artifact | Direction | Shape |
|---|---|---|
| `CommandRing` | in | one SPSC ring per endpoint; protocol in §5.1 |
| `View` | out, with a small return header back | 3 blocks, an exchange word, and a parallel reader-written `ret[3]`; stamped with its tick, row-filtered and converted to float at publish (§3.1, §3.2, §3.4, §3.5) |
| `EventRing` | out | bounded SPSC from the sim to the owner thread, fanned out per subscriber (§5.2) |

**`CommandRing`** (`design_python_api.md` §7.1):

| Aspect | Rule |
|---|---|
| Endpoint | **one SPSC ring per endpoint**, single-producer by contract. A source with several producer threads takes one endpoint per thread, or serializes itself. Source 0 is the host. The drain order between two endpoints of the same source is not yet specified ([Q62](open_question.md#q62-in-what-order-are-two-endpoints-of-one-source-drained)) |
| Submission | happens inside the submitting call, on the caller's thread. There is no relaying producer, no buffer-then-flush stage and no contention between sources: two endpoints never touch the same word |
| Result | two per command. **Admission** is returned by the call (`admitted \| queue_full \| too_late \| out_of_order \| over_margin \| invalid \| revoked \| host_error`). The **outcome** is read later, for a tick already released. Neither is an event |
| Tick | **the submitter always names it**, and the command runs at that tick or not at all. A local producer and a peer differ only in a declared stamp margin: 0, or the input delay |
| Capacity | the one number an endpoint declares: how many commands it may hold for **one tick**. There is no drain quota; a tick runs everything stamped for it, because pacing closed the set first |
| Order | the sequence is the position in the drain, so `(source id, sequence)` is a total order **by construction**: assigned by the engine, impossible to forge, and needing no sort |
| Record | recorded as consumed: exactly the commands the tick runs, so what is recorded is what was applied. Rejected commands never enter the record |

**`View`** (§3.1): stamped with its tick, converted to float at publish, and read-only for
every reader except for its return header (§3.5). Publishing is wait-free, one exchange, and
cannot fail. That is a synchronization property only. The projection copy it performs is
budgeted sim-thread work, a sum over registered Views, bounded per View by its row predicate
(§3.4).

**`EventRing`**: from the sim to the owner thread, bounded, and lossless up to its size. The
size is derived, `≥ C × D`, from the per-tick command ceiling and the drain interval the
ring is sized for. Overflow is a diagnosed condition, never a silent drop. A source cannot
cause it by submitting, because an application that stops draining pauses the simulation
first (§3.3, §5.2). This sizing assumes events are bounded by commands, which events the
engine creates itself are not
([Q10](open_question.md#q10-can-the-event-ring-fill-within-one-tick)). Fan-out to individual
subscribers may drop or merge events by delivery class (`design_python_api.md` §7.3):

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
  publish), partial by declaration (each View's spec, §3.1), and disposable (overwritten,
  latest wins; a missed publish loses nothing).
- Snapshots are **read-only for every reader**, and the publisher is the only writer. They
  are instances of the same generated world container, converted to float and generated
  without any mutating API (`design_data_container.md` §5.1). The publisher's own write path
  still needs a name ([Q40](open_question.md#q40-naming-the-publishers-write-path)). Tier 3
  core mods never read snapshots: they access live state inside their phases. Only a mod's
  async peripheral half reads a View, like any peripheral. A peripheral that wants writable
  data in the container's shape creates its own float-domain container: the same mechanism,
  a separate instance.
- **A snapshot is not a savegame.** The float conversion loses precision and the projection
  is partial, so core state cannot be rebuilt from snapshots. State is rebuilt by replay
  (the full artifact of §2.3, "seed plus command stream" in short). A future save/load
  feature must write the internal fixed-point state bit-exactly, as its own artifact.
- Desync checksums (§2.3) hash internal state, never snapshots. View cadence and content can
  neither cause nor hide a desync.

### 5.1 Command ring protocol (normative)

This section specifies the command ring with the five items of §1.1.

**Two facts support everything below, and they are one mechanism seen from two sides: every
command names the tick it applies to, and every producer is paced.** The gate (§3.3) does
not pass a tick until every source that acts in it has declared ready. No producer is
unpaced, and there is no path where a caller submits and the engine picks a tick afterwards:
a lower bound is the one answer a submitter cannot act on. How the host itself is paced is
open ([Q1](open_question.md#q1-how-is-the-host-paced)).

**An endpoint is single-producer.** That is a contract, not an observation. A source with
several producer threads registers one endpoint per thread at the freeze, or serializes
itself. In exchange, the inbound path is a plain SPSC ring: no CAS on the slot or index
path, no per-tick cells, no staging arena and no reclamation scheme. And no sort is needed
to order commands deterministically, because the drain order is the order. The endpoint
lease is a separate, cold lifecycle mechanism, because revocation is a second writer the
SPSC indices cannot represent.

**Endpoints declare a stamp margin** at the freeze, next to their capacity. This keeps local
and networked submission on one call with one shape: the difference between the host and a
peer is a number declared once, not a second API.

| Margin | Who | A command stamped for a later tick is |
|---|---|---|
| **0** | every local producer: the host, mods that submit commands, engine and AI sources. Each acts inside the tick it is about to release | **a bug**, rejected when submitted |
| the **input delay** | a peer, whose commands cross a network and are legitimately in flight across ticks | normal; this is why the margin exists (`design_multiplayer.md` §3.2) |

The transport owns a peer's margin, and its value is open
([Q50](open_question.md#q50-input_delay_ticks)). The engine imposes no value and no bound.

**State.** Per endpoint: two hot 8-byte ring atomics on separate cache lines, two cold
4-byte lease atomics, one cold waiter count, a plain slot array, and three words touched
only by the producer thread.

```
struct Entry { payload…; u64 tick; };        // the tick this command acts on

struct Endpoint {                            // one per producer, fixed at the freeze
    Entry            slot[DEPTH];            // DEPTH = (margin + 1) x capacity (design_limits.md §2)
    alignas(hardware_destructive_interference_size) std::atomic<u64> write;   // producer
    alignas(hardware_destructive_interference_size) std::atomic<u64> read;    // sim
    std::atomic<u32>  admission;             // OPEN | REVOKING | REVOKED
    std::atomic<u32>  active_submit;         // successful/validating calls in flight
    std::atomic<u32>  waiters;               // producers parked on the admission wait
    u32               capacity;              // commands this endpoint may hold for ONE tick
    u32               margin;                // 0 for every local producer
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
    if admission.load(acquire) != OPEN: return revoked
    active_submit.fetch_add(1, acq_rel)
    if admission.load(acquire) != OPEN:
        active_submit.fetch_sub(1, release); notify revoker; return revoked
    return LEASED                                      # LINEARIZATION POINT

leave:
    if active_submit.fetch_sub(1, release) == 1: notify revoker

revoke:
    admission.exchange(REVOKING, acq_rel)              # closes new leases
    wake every parked producer with `revoked`          # an admission wait is not a lease leak
    wait under the shutdown deadline for active_submit == 0
    admission.store(REVOKED, release)                   # revocation complete

open_or_reopen:
    require old producer stopped && active_submit.load(acquire) == 0
    admission.store(OPEN, release)                      # do not reset ring indices/slots
```

A lease that linearized before `REVOKING` completes normally, including publishing its entry
after revocation began; revocation waits for it. A later call returns `revoked` before
touching an index or slot. Revocation also **wakes every parked producer**. A producer
asleep on the admission wait holds a lease, and would otherwise be waited on for the whole
shutdown deadline; it wakes with `revoked` and leaves.

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
    if t <  fu:            leave(); return too_late                    # (C1a) STRICTLY past
    if t <  last_stamped:  leave(); return out_of_order                # (C1b)
    if t >  fu + margin:   leave(); return over_margin                 # (C1c)
    if t != last_stamped:  last_stamped = t; stamped_for_tick = 0      # producer-local
    if stamped_for_tick == capacity: leave(); return queue_full        # (C1d)
    w = write.load(relaxed)                                            # (C2) our own word
    while w - read.load(seq_cst) == DEPTH:                             # (C3) THE ADMISSION WAIT
        waiters.fetch_add(1, seq_cst)                                  #      arm, then re-read
        if w - read.load(seq_cst) < DEPTH: { waiters.fetch_sub(1, relaxed); break; }
        park until woken; on `session_paused` retry, on `revoked` leave and return revoked
        waiters.fetch_sub(1, relaxed)
    slot[w % DEPTH] = cmd; slot[w % DEPTH].tick = t                    # (C4) plain; the slot is ours
    ++stamped_for_tick
    write.store(w + 1, release)                                        # (C5) LINEARIZATION POINT
    leave(); return admitted{ handle }
```

Whether the admission wait (C3) can ever be reached is open
([Q8](open_question.md#q8-can-the-admission-wait-ever-happen)).

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
        st = slot[r % DEPTH].tick
        if st > t: break                                      # (S2) stamped for a later tick
        if st < t: protocol_error(e, st, t)                   # (S3) NEVER executed late
        emit(slot[r % DEPTH], source(e), seq++); ++r
    e.read.store(r, seq_cst)                                  # (S4)
    if e.waiters.load(seq_cst): wake e's parked producers     # (S5) §3.3's arming discipline
record; execute                                               # §2.3, §6
first_unexecuted.store(t + 1, release)                        # (S6)
```

**Everything stamped for `t` runs at `t`. There is no quota and no per-tick limit on the
count.** That is what makes the drain deterministic, not just bounded. Pacing closes the set
before the drain runs. When the gate opens for `t`, every participant has already submitted
its commands for `t` and declared ready. Anything submitted afterwards is stamped `t + 1` or
later, because `first_unexecuted` has not moved. **The tick label makes the cut, not a
count**, and a count is exactly what two machines could disagree about.

Five properties follow from the mechanism:

- **The drain never waits for a producer.** (S1) reads whatever index the producer has
  published and stops there. The engine's wait for that producer is the gate's: declared,
  and finished before the drain ran (§3.3). No slot is ever reserved but unfilled, because a
  slot becomes visible and complete in the same operation, (C5).
- **A producer waits only for space, never for a tick to finish.** (C3) is the only
  interaction, and it lasts at most one tick. The sim frees a tick's worth of entries every
  tick, and (C1d) has already confirmed that this producer is within its own allocation.
- **Ordering is total, deterministic and cannot be forged.** The drain assigns `seq` in a
  fixed endpoint order over each endpoint's FIFO. So `(source id, sequence)` is a total
  order with no sort and no field a source could forge. Order within one source is the
  producer's own.
- **A command runs at the tick it named, or not at all.** (S2) stops the drain at the first
  entry for a later tick. (S3) refuses to run one whose tick has passed. There is no third
  outcome where a command silently moves.
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
| Who | every producer, peers included | every producer |
| Why | the alternative is dropping a command, and for a peer a dropped command is a certain desync | feedback, and the only place a rejection at consumption is reported |
| Length | until space frees: at most one tick | until the named tick has run |
| Can it fail? | only if the producer breaks its own contract | **no.** It *reports* failures; it is not one |

```
h = submit(cmd, t)        # admission: it is in the queue for t
...
outcome(h)                # what it did at t: applied, or a named application-time rejection
```

For a peer the outcome is worth having even though it is identical on every machine: it
shows trouble early, and it gives the transport a way to push back.

**The deadlock rule.** A deadlock can happen only if something waits for the outcome of a
tick it is itself holding up. The right order of operations removes it:

```
submit commands for tick t     # admission
declare ready for tick t       # releases the tick
...the engine drains, runs, advances...
read outcomes for tick t       # a tick already released
```

**A producer may wait only for outcomes of ticks it has already released.** The engine
enforces this rather than just documenting it. It knows which participant is asking and
which tick that participant holds, so a wait for the outcome of an unreleased tick is
refused at once, naming the tick. A clear error at the call site is far better than a frozen
session.

Admission cannot deadlock either, for a simpler reason. An endpoint's capacity is at least
one tick's worth of its own commands, so a producer submitting within its allocation always
fits. (C1d) marks that boundary, and crossing it is the contract violation, not the wait.

**There is exactly one capacity rejection.** `queue_full` means "more than you allocated for
this tick": a producer breaking its own declaration, not a busy engine. A busy tick, a slow
drain or a paused session causes a wait, not a rejection. So every rejection is either a
contract violation, a lifecycle state, or a transport failure, and none is a load signal:

| Result | Class | Means |
|---|---|---|
| `admitted{handle}` | none | in the ring, for the tick you named. The handle reads the outcome |
| `queue_full` | contract | more than `capacity` commands stamped for one tick on this endpoint |
| `too_late` | contract | the named tick is **strictly** in the past |
| `out_of_order` | contract | the named tick is earlier than one already named on this endpoint |
| `over_margin` | contract | further ahead than this endpoint's declared margin; for a local producer, any tick but the current one |
| `invalid` | contract | malformed payload, or a capability the source does not hold |
| `revoked` | lifecycle | this endpoint is not admitting: torn down, or the session is in playback |
| `host_error` | transport | a process host's IPC round trip failed or exceeded `ipc_deadline`; process hosts only (`design_modding.md` §4.3) |

Whether `session_paused` also belongs in this table is open
([Q7](open_question.md#q7-does-a-caller-ever-see-session_paused)).

**`too_late` rejects only ticks already in the past.** That is one tick narrower than it
might seem, and the excluded tick is the one nearly every command lands in. A command
stamped for the tick the engine is currently waiting on is the normal case, not a late one.
`first_unexecuted` has not moved and cannot, because the producer that would release that
tick is the one submitting. Pacing, not timing, makes the current tick safe. A peer's
commands for `t` arrive before it declares ready for `t`, and the gate cannot pass `t` until
it does. So in a healthy session `too_late` cannot happen; if it does, it reports a defect,
not load.

**Capacity is the only number an endpoint declares:** how many commands it may hold for one
tick. There is no second number for the drain to cap, because a tick runs everything stamped
for it. The ring depth follows:

| Endpoint | Depth |
|---|---|
| margin 0 | `capacity` |
| a peer at margin `m` | `(m + 1) × capacity`: one tick's worth for each tick legitimately in flight |

The engine-wide per-tick ceiling is `C = Σ capacity(e)` over registered endpoints
(`design_limits.md` §2). It is a bound checked at the freeze, not an allocation, which is
why the limit on sources can be generous at no cost to a session with four sources. The
event ring's size, `C × D`, follows from it (§5).

**Memory orders.**

- (C5) `release` pairs with (S1) `acquire`: one edge per publish, making every entry up to
  `w` visible.
- (S4) `seq_cst` pairs with the load in (C3). This makes a slot safe to overwrite: the sim's
  reads of it happen before the producer's next write to it. It is `seq_cst` rather than
  `release` for the same reason as the gate in §3.3. (S4)/(S5) and (C3)'s arm and re-read
  are a store followed by a load on each side of the same two objects, and only a single
  total order rules out a lost wake-up in both directions.
- (C1a)'s load of `first_unexecuted` is `acquire`, pairing with (S6).
- (C2), (C4) and each side's load of its own index are relaxed or plain.

**Progress.** An uncontended submit is wait-free: two lease loads, two active-count
read-modify-writes, three bounded validity checks, one relaxed index load, one `seq_cst`
load, a copy and one release store, with no loop. A submit that finds the ring full is a
**bounded wait** in the sense of §1.1: declared, bounded by one tick, and with no expiry on
the normal path. The only other ways out are the two named wake reasons, `session_paused`
(retry) and `revoked` (leave). The drain is `O(commands stamped for this tick)` with no
retries. The ring indices are never contended, since the producer owns `write` and the sim
owns `read`. Revocation contends only on the separate endpoint lease.

**Failure and deadline behaviour.** Every rejection in the table above is returned
synchronously, and the command is not enqueued. The admission wait has no deadline of its
own, on purpose: its bound is structural (one tick), not a timer. A session that is not
advancing has paused, and a pause wakes the waiter with a reason instead of timing it out
(§3.3). Revocation waits, under the shutdown deadline, for leases that already linearized.
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
- **Pause**: a pause taken while producers are parked wakes every one of them with
  `session_paused`, and a resume needs no second wake.

The wake-up parts of these races (a parked producer woken by revocation, by a drain, or by a
pause) are properties a sanitizer cannot fully check
([Q6](open_question.md#q6-how-are-the-wake-up-handshakes-tested)).

### 5.2 Event ring (normative)

The event ring is a bounded single-producer, single-consumer ring. The sim thread enqueues,
and the owner thread drains it in `drain_events()` (`design_python_api.md` §6 gives it
exactly one owner for this reason). Publication uses the idiom of §1.1: fill the entry, then
release-store the write index. The drainer's acquire load of that index makes the entry
visible. It has the same shape as the ring of §5.1, with the direction reversed.

**Why it has exactly one producer.** The backlog rule below depends on this. Events are
emitted at commit points, and commit points are single-threaded: the staged merge at every
phase close and the terminal commit both run sequentially in fixed task order (§4.1). That
order is the determinism guarantee, so it cannot be relaxed. The one part of a commit that
runs on the worker pool, the compaction scan and gather, emits nothing; it is a permutation.
Anything that needs to report from inside a parallel phase uses the same route as log
records. It writes to a per-worker buffer, which the single thread running the phase
boundary drains in worker order (`design_patterns.md` §7). So the ring has one producer by
construction, not by convention, and no emission site needs auditing for it.

**The event backlog has one owner.** `backlog` counts the entries the owner thread has not
yet consumed. It has exactly one writer, the sim thread, which is the only thread that adds
entries. It is a plain `u64`, not an atomic and not a flag, because the other thread never
touches it:

```
enqueue:           ++backlog                              # sim thread, plain
before the gate:   backlog = write - read.load(seq_cst)   # sim thread; `read` is the owner's
                                                          #   ordinary SPSC consumer index
blocker_for(t):    if backlog >= high_water: HOST_BACKLOG  # a plain local comparison (§3.3)
```

The owner publishes nothing new for this. `read` is the index it already stores as the
ring's consumer, and the sim already loads it. The backlog is recomputed from two indices
that only increase, at the one place the sim already evaluates the gate. A drain that clears
the condition must wake a parked sim, and an unparked sim must never be signalled, using the
arming rule of §3.3. The exact store order of `read`, and where the recompute sits relative
to arming, are open ([Q11](open_question.md#q11-how-does-draining-events-wake-the-sim)).

**Why a derived value and not a cached flag.** A flag looks cheaper, but cannot be made
correct. Both threads would have to write it: the enqueue sets it and the drain clears it,
and each writes based on a count read before the other wrote. The drain clears it. An
enqueue holding a count from before the drain sets it again. The engine then blocks on a
backlog that does not exist, until some later enqueue happens to re-evaluate it. A recheck
fixes one direction and not the other, because two writers deciding one fact from two stale
readings is a mechanism with two owners, not a race to patch. One owner of a derived value
removes the whole class of bug.

`high_water` belongs to the ring. Its value is open
([Q63](open_question.md#q63-what-is-high_water)): the threshold is a measurement question,
separate from where the counter lives and who owns it.

**Overflow: who is at fault decides the response.** These three cases are the engine's whole
overflow policy. They are kept apart so that the mildest fault does not get the harshest
response:

| Queue | At fault | Response |
|---|---|---|
| a mod's own inbox | that mod is not draining | **suspend that mod** and report; the simulation continues (`design_modding.md` §4.2) |
| the event ring | the application is not draining | **pause the simulation** and report: `HOST_BACKLOG` above |
| a peer falling behind | never decided locally; the server tier decides (§3.3, `DROP`) | until the server tier decides: **pause and report** |

None of these ends a session. Each pause is undone by whatever caused it: the application
drains and the gate opens, or the server tier answers and the drop applies at its agreed
tick. **Suspension is reversible too.** The mod stops being fed, keeps its endpoint, and
resumes once it drains (`design_modding.md` §4.2). Unloading a suspended mod is a separate,
deferred question ([Q47](open_question.md#q47-unloading-a-suspended-mod)).

`D` means the same in every mode, and the `C × D` size still holds under fast-forward.

## 6. Agent-based systems: hybrid mind/body split

An agent has a **body**: position, resources, health, anything the world tracks. The body is
core state and updates deterministically. Where the agent's **mind** lives is a choice of
determinism boundary, and it decides what a replay needs:

- **Mind outside the boundary** (an async float or GPU brain that emits commands): the
  simulation is deterministic given the recorded command stream, but cannot be reproduced
  from the seed alone. Agent commands must be recorded exactly like player input. Replay
  size grows with agent count times decision rate, and lockstep multiplayer would need an
  authority to compute and broadcast agent commands.
- **Mind inside the boundary** (fixed-point decision logic, core-seeded RNG, deterministic
  decision ticks): replay from the seed works with nothing recorded per agent. But no float
  math is allowed, and GPU inference inside the boundary is impractical (driver variance,
  §5).

**The design is a hybrid:**

- **Inside the core (deterministic)**: cheap, frequent, per-tick agent logic such as utility
  scoring, steering, state machines, pathfinding and reflexes. This is simple math anyway,
  runs in parallel under §4 like any other system, and keeps replays small.
- **Outside, as peripherals (float or GPU, async)**: expensive, occasional thinking such as
  planning, learned policies and batched neural-net inference. These read snapshots, think
  at their own pace, and emit commands that are recorded into the input stream.
  Architecturally this is the same as a human player: a non-deterministic brain whose
  signals are deterministic inputs.

Rules that keep this sound:

- **The core resolves agent commands deterministically**: validated, ordered by `(source id,
  sequence)`, and conflicts settled by fixed rules, never by arrival time.

  | Aspect | Rule |
  |---|---|
  | order key | `(source id, sequence)`, both **assigned by the drain** (§5.1): the endpoint order and the position within it. Nothing is sorted, because nothing arrives out of order |
  | why the engine assigns it | a sequence supplied by the source need be neither unique nor increasing, so ties would resolve by arrival time: an iteration-order desync (§2.2) disguised as a sort |
  | order within one source | still the source's own: an endpoint is a FIFO, so `submit_batch` is `k` submits and ordering the batch orders the commands |
  | naming | a permanent **id**, never a row offset. A row offset from the snapshot of tick N means nothing at tick N+k (`design_data_container.md` §2.2) |
  | resolution | id to row, by a lookup whose shape depends on the object type's storage policy: binary search where the id column is sorted, the type's index where it is not (`design_data_container.md` §2.2). **If the id is not found, the entity is gone** and the command is deterministically rejected. That is reaction latency, never an error or undefined behaviour |

  The last row is a positive identity test: a command can never silently apply to a
  different entity from the one it named.
- **The submitter names the tick a command applies at, and the engine either keeps that
  promise or refuses the command** (§5.1). Nothing is moved to a different tick. The drain
  runs what is stamped for the current tick, refuses what is stamped for a tick already
  gone, and leaves a later stamp alone. So a caller is never told a tick that later changes.
  An async mind that takes three ticks to decide names the tick it is deciding for, and the
  engine only asks whether that tick can still be reached.

  The gate makes this more than bookkeeping. Every instance waits for `t` rather than
  running past it (§3.3), so a named tick means the same tick everywhere. A lockstep peer
  needs this (`design_multiplayer.md` §3.2), and a local caller gets it for free. How far
  ahead of the current tick an endpoint may name is its declared stamp margin: 0 for every
  local producer, the input delay for a peer. The transport chooses that delay; the engine
  imposes no value and no bound.
- **Replay feeds commands in at consumption, not at submission.** The recorded stream is
  already ordered and already carries each command's tick (§2.3, §5). So a loaded replay
  hands each tick its commands directly and closes every endpoint while it runs: every live
  submission, from every source, is refused (`design_python_api.md` §4.2). Otherwise live
  commands could be mixed into a replayed stream, and the checksum would diverge for reasons
  nothing records.
- **Async minds working from a tick-N snapshot get natural reaction latency.** The mind
  names the tick it can actually make, so slow inference costs the agent responsiveness
  rather than costing the simulation its tick rate. It does not free the mind from the gate:
  a mind that submits is a participant (§3.3), so the tick it names is one the session waits
  for. A mind that cannot meet its own deadline should name a later tick, which is the
  honest form of the same trade. A mind that only watches submits nothing and paces nothing.

## 7. Suggested build order

Each step is labelled with the milestone it belongs to (see `glossary.md`).

1. **M1: session lifecycle, tick loop, the gate, command rings and one View**, with no
   simulation content. The `configuring → freeze → running` sequence (§2.4) belongs here
   rather than later. Identity, column ids, source ids, Views and participants are all fixed
   at the freeze, so every later step is written against a session that already exists. When
   the host is the only participant, the gate never blocks, so this step builds the gate at
   its simplest setting.
2. **M2: the `fixed<>` type, deterministic PRNG, per-tick checksum, and replay record and
   playback.** The verification harness must exist before the first system does.
3. **M3: the SoA world container**, with chunked columns, permanent ids over tick-scoped row
   offsets, and SIMD-friendly alignment.
4. **M4: the phase-structured scheduler**: explicit system list, per-column access
   declarations, the phase cut and its invariant check, staged buffers, fixed-order commit.
   Prove that 1 thread and N threads give the same checksums.
5. **M5 and M6: the first real system, and a renderer reading a View with the `ALL`
   predicate**; then grow. A row predicate (§3.4) is added when `publish bytes/second` calls
   for one, not before.

## Appendix A. The `SHARED` View (deferred)

**Not built in v1.** This appendix specifies the multi-reader View mode, kept because the
analysis is sound and one kind of reader will need it. It describes the design in the
present tense; nothing here is built until process-host mods are.

**Why it is deferred.** A `SHARED` reader cannot keep a block, so it copies out. N readers
therefore cost N full copies of the projection, on top of the publisher's one. At the row
counts `design_limits.md` §5 budgets for, that is the largest cost in the whole engine. One
`SHARED` View with three readers over 10⁷ rows at a 32-byte spec moves 1.28 GB per tick.
Three `PRIVATE` Views over their own declared columns and rows move a fraction of that. The
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
([Q60](open_question.md#q60-reviving-the-shared-view)). Readers are known at the freeze,
which removes most of what makes general hazard pointers hard.

**What needs it.** A process mod host, which cannot be given a `PRIVATE` View: a `PRIVATE`
publisher uses the block index its reader wrote (§3.2), so a hostile reader could steer a
payload address. `SHARED` reads the block-state words the child process writes only as
availability, and never derives an address, index, generation or loop bound from them. The
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
and each publish can leave one older generation pinned without waiting. Four blocks give
**three generations of slack**. This is not an assumption that a copy finishes within 66 ms:
an arbitrarily slow copy stays safe, and if all three candidates are pinned, that View skips
one due publish.

**State.**

```
struct SharedView {
    Block*            block[4];
    std::atomic<u64>  published;      // (generation << 2) | index
    std::atomic<u32>  state[4];       // bit 31 WRITING; low 31 bits reader count
    u32               cursor;         // publisher-private scan start
    u64               generation;     // publisher-private; 62 bits on publication
};
```

The cursor and generation are plain variables touched only by the publisher. Running out of
generations is a terminal invariant failure, checked before wrap. At 30 Hz, 2^62 publishes
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
          grow/replace *block[chosen] if needed              # (S3) claim excludes readers
          fill *block[chosen]                                # (S3) plain
          state[chosen].store(0, release)                    # (S4) payload/ptr complete
          ++generation; assert(generation < 2^62)
          published.store((generation << 2) | chosen, release) # (S5) LINEARIZATION POINT

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
an exact match at (R3) shows that the pinned block is still the published generation. Only
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
that View after its bounded scan, leaving the previous immutable snapshot published. This is
sampling, not a session failure, and emits no event per skip. `publish_skipped_pinned`
counts skips, and a sustained nonzero rate identifies a reader holding its pin too long. A
`SHARED` View may not be paced: it is sampled and may skip, so `paced=True` is a
registration error rather than a promise the mechanism cannot keep.

**Growth.** A successful `0 → WRITING` claim proves that no reader can have loaded that
block's pointer, so the publisher may free and replace the block in place. A reader loads
`block[i]` only after pinning and revalidating, so the pointer is never read and written at
the same time.

**Verification.** The sanitizer test checks three things. No ordinary pointer or payload
access happens without a successful pin and revalidation. A writer never claims a block with
a nonzero reader count. And when all three candidates are pinned, the publish skips without
touching a pinned block. A long reader spanning many publishes, and a race between growth
and replacement, are required cases. A protocol that validates after copying cannot express
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
