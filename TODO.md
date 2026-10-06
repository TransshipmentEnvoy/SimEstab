# TODO

The roadmap, by milestone. Milestones are defined in [doc/glossary.md](doc/glossary.md), and
the design each item implements is in `doc/`. Unresolved design questions are in
[doc/open_question.md](doc/open_question.md); items that depend on one link to it.

**Where things stand.** Built: the `error`, `log`, `gpu`, `viz`, `util` and `limits` modules.
That covers the root of the exception family, SDL3 compute and viz contexts, logging, the SDL
main-thread init guard, and the decided limits (M0). The toolchain the designs assume is in
place: GCC 16, C++26 with `-freflection`, and `-march=x86-64-v3`. M1 is under way: the engine
view protocol, the gate and the tick loop are written and tested, and nothing reaches them
from Python yet. Everything after M1 is designed but not written.

+ [x] basic setup
+ [x] graphics basics
  + [x] basic SDL3
  + [x] basic compute context
  + [x] basic viz context
  + [x] storage-buffer layout for scalar columns: std430 with scalar stride confirmed on
        Intel Mesa ANV and lavapipe (`doc/design_data_container.md` §5)
+ [x] logging subsystem (C++ triad, console sink, Python bindings;
      `doc/design_logging.md` §1)
+ [x] **M0: decided limits in code.** Every value in `doc/design_limits.md` is in
      `sim_estab:limits` or `src/sim_estab/config.py`, with tests in `test/test_config.py`
+ [ ] **M1: session, engine views, command ring, gate.** No simulation content yet: a block
      and a command ring entry carry a tick and opaque bytes. M1 runs headless: `step(n)`
      runs ticks on the calling thread (`doc/design_engine_core.md` §7 step 1). No open
      question blocks it
  + [ ] session lifecycle: `configuring → freeze → running → stopped`, with the freeze's
        ordered steps; every step that can fail runs before any step with a visible effect
        (`doc/design_engine_core.md` §2.4, `doc/design_python_api.md` §2)
  + [ ] the headless executor: `step(n)` runs the tick loop on the calling thread and
        returns at its own grant or a stop. It checks for signals at tick boundaries, so
        Ctrl+C ends the step (`doc/design_engine_core.md` §3.3,
        `doc/design_python_api.md` §4.3)
  + [ ] the gate: one `ready_through` per participant (only ever increases), the gate mutex
        with a condition variable for blocking and deadlines, and `first_unexecuted` as the
        next tick to run, stored under the mutex after each publish. Every gate input
        changes under the mutex, followed by a notify (the wake-up rule). The host controls
        it with two independent atomics, `run_until` and `stop_requested`; pause, resume and
        step serialize under the same mutex. The host is not a participant, so in M1 the
        gate ends a step, stops it at the backlog mark, and waits only for paced engine
        views (`doc/design_engine_core.md` §3.3)
  + [ ] the unpaced host endpoint: `submit(cmd)` names no tick and never waits; a full ring
        returns `queue_full`; `outcome(h)` returns `pending`, then the outcome and the tick
        that ran it (`doc/design_engine_core.md` §5.1)
  + [ ] the engine view, `PRIVATE` only: 3 blocks and one `u32` exchange word, publish and
        take both `acq_rel`. The permutation invariant makes block reuse safe without
        reclamation. A due publish always has a writable block and is never skipped.
        Registered at the freeze with a projection spec, `identity` (the `id` column, on by
        default), a row predicate kind (only `ALL` in M1) and a cadence; may be paced, and
        then its take declares the reader ready through the next publish
        (`doc/design_engine_core.md` §3.1, §3.2). A stepped tick publishes only the engine views
        due by cadence. The multi-reader `SHARED` mode is specified but not built (Appendix
        A)
  + [ ] the on-demand snapshot: `snapshot()` on the owner thread, while no tick can run,
        refills the one snapshot buffer with every `[[=viz]]` column and `id` in one pass;
        a call while array views of the previous result are alive raises `EngineViewBusyError`
        (`doc/design_engine_core.md` §3.1)
  + [ ] the return header: a fixed `ret[3]` next to the blocks, written by the reader and
        read by the publisher, carrying predicate parameters, `last_consumed_tick` and a
        cadence hint. It needs no new atomic: the take exchange already publishes it. The
        publisher adopts only a newer `seq`. It may shape the engine view and never reaches
        the world (`doc/design_engine_core.md` §3.5)
  + [ ] the command ring: one SPSC ring per endpoint and one endpoint per source, drained in
        ascending source id at the start of each tick; the sequence is the drain position,
        so there is no sort and no ordering field a source could forge. Every paced command
        names the tick it applies to, and each paced endpoint declares how far ahead it may
        name. Depth is `capacity`, or `2 × capacity` for an endpoint with a margin, which
        then waits for space on the endpoint's mutex until its oldest tick has run.
        `too_late` covers a tick already released. There is no drain quota, so `queue_full`
        only means a producer exceeded its own per-tick capacity. The slot size is a
        provisional constant until the payload schema (M2)
        (`doc/design_engine_core.md` §5.1)
  + [ ] the endpoint lease: revoke closes admission, wakes producers parked on the
        admission wait or an outcome wait, and waits for active submits before reclaim or
        reopen; a retry keeps the same ring (`doc/design_engine_core.md` §5.1)
  + [ ] the event ring and the event backlog, sized `E × (D + 1)` with the mark at `E × D`;
        the side list for events raised outside a tick; the two event drains, declared by
        `EngineConfig.event_drain`: the owner drain and the native sink; and
        `EventBacklogError` from a step that reaches the mark under an owner drain
        (`doc/design_engine_core.md` §5.2)
  + [ ] the M1 events: `session.*`, `sim.backlog_*`, `participant.*`,
        `command.protocol_error` and `engine_view.bandwidth_over`
        (`doc/design_python_api.md` §7.3)
  + [ ] operation leases around every native call that can overlap `close()`:
        `OPEN → CLOSING` rejects new calls, running calls drain under the shutdown
        deadline, and a timeout enters `failed` and disarms rather than freeing native
        resources under a caller. `on_failed_stop` covers every `failed` path; a lost GPU
        device or a failed page commit exits the process (`doc/design_python_api.md` §2, §6)
  + [ ] the Python surface: `engine.py`, `command.py`, `event.py`, `loop.py`, with the
        headless loop of `doc/design_python_api.md` §4.2. `WindowConfig`, `LogConfig` and
        `event_drain` join `config.py` (`doc/design_python_api.md` §1, §3)
  + [ ] logging management, the remaining parts: the refcounted Python wrapper under a
        module-level lock, and the per-session file sink in `LogConfig.session_dir`
        (`doc/design_logging.md` §2, §3)
  + [ ] sanitizer tests for both protocols: the engine view's permutation invariant,
        including the reverse edge from reader to publisher, and a return header written
        before a take arriving intact at the publisher that receives the block, never while
        the reader is still writing it; endpoint revoke racing submit; `close()` racing each
        any-thread API; and every `admitted` command appearing exactly once in drain order.
        A functional test cannot tell a correct implementation from a broken one on x86-64
  + [ ] stress tests for every park under a watchdog timeout: the gate against
        declarations and control calls, tick progress against the executor, and the
        admission wait against the drain. Lost wake-ups are hangs, not data races
        (`doc/design_engine_core.md` §3.3)
  + [ ] a test that the defaults in `config.py` and `sim_estab:limits` agree
+ [ ] **M2: fixed-point, PRNG, checksum, replay** (`doc/design_engine_core.md` §2, §7 step 2)
  + [ ] the `fixed<>` type ([Q25](doc/open_question.md#q25-the-fixed-specification))
  + [ ] the PRNG and per-system streams ([Q26](doc/open_question.md#q26-the-prng))
  + [ ] the three checksum levels: the tick digest, the rolling checksum
        ([Q67](doc/open_question.md#q67-the-rolling-checksum-period-and-the-tick-digest)),
        and the full checksum with per-system hashes. The algorithm is open
        ([Q27](doc/open_question.md#q27-the-checksums-exact-algorithm-and-inputs))
  + [ ] replay record and playback, with the replay header checked at the freeze
+ [ ] **M3: the world container** (`doc/design_data_container.md`)
  + [ ] build the reflection-in-module shape (`define_aggregate` in a `.cppm`, exported as
        concrete types) through the Conan/CMake module build; the spike only used bare
        `g++-16 -fmodules` (`doc/design_data_container.md` §8)
  + [ ] the reflection-generated pool: chunked columns with address space reserved at the
        cap, the live bitmap, chunk live counts, the generation column, the retired bitmap
        and the extent, with SIMD-friendly alignment (`doc/design_data_container.md` §2.2)
  + [ ] generational ids (`slot << 32 | generation`) with O(1) lookup
  + [ ] the terminal commit's two steps: erases (clear the live bit, bump or retire the
        generation, zero the row), then creates in merge order into the lowest free slots
        (`doc/design_data_container.md` §2.2)
  + [ ] `[[=append_only]]` and `[[=derived]]` (`doc/design_data_container.md` §2.1)
  + [ ] save and load of the layout: extent, live and retired bitmaps, and generations, so
        a loaded session continues exactly as one that never saved
+ [ ] **M4: the phase scheduler** (`doc/design_engine_core.md` §4.1)
  + [ ] the phase cut: per-column access declarations, the phase invariant check, and the
        greedy cut over the fixed system order
  + [ ] two commit kinds, kept apart: the boundary commit (staged merge and `write_dbl`
        flip, at every phase close) and the terminal commit (all structural changes, once
        per tick). Creation is deferred: an entity created this tick has no row until the
        terminal commit
  + [ ] prove that 1 thread and N threads give the same checksums
+ [ ] **M5: engine view projection, row predicate, GPU upload**
  + [ ] the row predicate kinds beyond `ALL` (`AABB`, `SPHERE`, `FRUSTUM`, `TAG`),
        evaluated to a mask and reusing the publish scan and gather. Coarse in the engine core,
        exact in the reader; a filtered engine view carries the id column. Added when publish
        bandwidth calls for it (`doc/design_engine_core.md` §3.4)
  + [ ] advertise SPIR-V only at device creation. The scope is Vulkan only; today DXIL is
        advertised while `create_pipeline` hard-codes SPIR-V, so selecting D3D12 fails later,
        at pipeline creation
  + [ ] `ComputeContext` upload API for the snapshot path: a reusable per-frame staging
        transfer buffer (mapped and unmapped every frame), one cycled destination buffer
        per column rewritten whole, fence handles, and a `last_uploaded_snapshot_tick` per
        destination advanced when its fence signals (`doc/design_data_container.md` §5).
        GPU buffers are sized per engine view from its maximum rows; how an engine view declares that
        maximum is open ([Q39](doc/open_question.md#q39-the-gpu-allocator-for-growing-worlds))
  + [ ] port the std430 spike into `libsim_estab_viz_test` as a regression test; a silent
        stride change would corrupt every frame with no error
+ [ ] **M6: first system, renderer, sim-thread mode** (`doc/design_engine_core.md` §7 step 5,
      `doc/design_python_api.md` §4.3)
  + [ ] the sim thread: `run_sim_async()`, and `stop_sim_async()` with the staged join into
        `stopped`. Every windowed session runs it; a headless one may, for real time. The
        frame loop never waits on the engine; it polls (`doc/design_python_api.md` §4.1,
        §4.3)
  + [ ] `step(n)` on a paused sim thread: no deadline, waiting in slices that check for
        signals, so Ctrl+C revokes the grant (`doc/design_python_api.md` §4.3)
  + [ ] async error handoff: state `EMPTY → WRITING → READY`; claim before writing the slot,
        release-publish only at `READY`, acquire-read only from `READY`
        (`doc/design_python_api.md` §8), with its sanitizer test of racing writers
  + [ ] the drain wake: `drain_events()` stores `read` under the gate mutex and wakes a sim
        parked on the backlog (`doc/design_engine_core.md` §5.2)
  + [ ] the catch-up clamp, `catch_up_clamp_seconds = 0.25` in `sim_estab:limits`, and
        `sim.behind` (`doc/design_limits.md` §1.2)
+ [ ] **M7: mod tiers** (`doc/design_modding.md`); process hosts ship without snapshots.
      How a mod host thread gets a snapshot on request is open
      ([Q84](doc/open_question.md#q84-how-are-snapshot-and-checksum-opened-to-other-threads))
