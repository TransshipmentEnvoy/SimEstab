# TODO

The roadmap, by milestone. Milestones are defined in [doc/glossary.md](doc/glossary.md), and
the design each item implements is in `doc/`. Unresolved design questions are in
[doc/open_question.md](doc/open_question.md); items that depend on one link to it.

**Where things stand.** Built: the `log`, `gpu`, `viz`, `util` and `limits` modules. That
covers SDL3 compute and viz contexts, logging, the SDL main-thread init guard, and the
decided limits (M0). The toolchain the designs assume is in place: GCC 16, C++26 with
`-freflection`, and `-march=x86-64-v3`. Everything from M1 on is designed but not written.

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
+ [ ] **M1: session, engine views, command ring, gate.** No simulation content yet
      (`doc/design_engine_core.md` §7 step 1)
  + [ ] session lifecycle: `configuring → freeze → running`, with the freeze's ordered
        steps; every step that can fail runs before any step with a visible effect
        (`doc/design_engine_core.md` §2.4)
  + [ ] the gate: one `ready_through` per participant (only ever increases), one counting
        semaphore for blocking and deadlines, and `first_unexecuted` as the next tick to
        run. The host controls it with two independent atomics, `run_until` and
        `stop_requested`; the event backlog is a plain value owned by the sim. Pause,
        resume and step serialize so one cannot overwrite another. When the host is the
        only participant, the gate never blocks (`doc/design_engine_core.md` §3.3). How
        the host is paced is open
        ([Q1](doc/open_question.md#q1-how-is-the-host-paced))
  + [ ] the engine view, `PRIVATE` only: 3 blocks and one `u32` exchange word, publish and take
        both `acq_rel`. The permutation invariant makes block reuse safe without
        reclamation. A due publish always has a writable block and is never skipped.
        Registered at the freeze with a projection spec, a row predicate kind (only `ALL`
        in M1) and a cadence; may be paced (`doc/design_engine_core.md` §3.1, §3.2). The
        multi-reader `SHARED` mode is specified but not built (Appendix A)
  + [ ] the return header: a fixed `ret[3]` next to the blocks, written by the reader and
        read by the publisher, carrying predicate parameters, `last_consumed_tick` and a
        cadence hint. It needs no new atomic: the take exchange already publishes it. The
        publisher adopts only a newer `seq`. It may shape the engine view and never reaches the
        world (`doc/design_engine_core.md` §3.5)
  + [ ] the command ring: one SPSC ring per endpoint, drained in ascending source id at the
        start of each tick; the sequence is the drain position, so there is no sort and no
        ordering field a source could forge. Every command names the tick it applies to,
        and each endpoint declares how far ahead it may name (0 for local producers, the
        input delay for a peer). Depth is `(margin + 1) × capacity`. There is no drain
        quota, so `queue_full` only means a producer exceeded its own per-tick capacity
        (`doc/design_engine_core.md` §5.1). Whether the admission wait is reachable is open
        ([Q8](doc/open_question.md#q8-can-the-admission-wait-ever-happen))
  + [ ] the endpoint lease: revoke closes admission, wakes producers parked on the
        admission wait, and waits for active submits before reclaim or reopen; a retry
        keeps the same ring (`doc/design_engine_core.md` §5.1)
  + [ ] the event ring and the event backlog (`doc/design_engine_core.md` §5.2). How a drain
        wakes a sim parked on the backlog is open
        ([Q11](doc/open_question.md#q11-how-does-draining-events-wake-the-sim))
  + [ ] operation leases around every native call that can overlap `close()`:
        `OPEN → CLOSING` rejects new calls, running calls drain under the shutdown
        deadline, and a timeout enters `failed` and disarms rather than freeing native
        resources under a caller (`doc/design_python_api.md` §2, §6)
  + [ ] async error handoff: state `EMPTY → WRITING → READY`; claim before writing the slot,
        release-publish only at `READY`, acquire-read only from `READY`
        (`doc/design_python_api.md` §8)
  + [ ] the Python surface: `engine.py`, `command.py`, `event.py`, `loop.py`
        (`doc/design_python_api.md` §1)
  + [ ] logging management, the remaining parts: the refcounted Python wrapper and the
        per-session file sink (`doc/design_logging.md` §2, §3)
  + [ ] sanitizer tests for both protocols: the engine view's permutation invariant, including the
        reverse edge from reader to publisher, and a return header written before a take
        arriving intact at the publisher that receives the block, never while the reader is
        still writing it; endpoint revoke racing submit; `close()` racing each any-thread
        API; async-error writers racing; and every `admitted` command appearing exactly once
        in drain order. A functional test cannot tell a correct implementation from a broken
        one on x86-64. Lost wake-ups need more than a sanitizer
        ([Q6](doc/open_question.md#q6-how-are-the-wake-up-handshakes-tested))
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
        evaluated to a mask and reusing the publish scan and gather. Coarse in the core,
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
+ [ ] **M7: mod tiers** (`doc/design_modding.md`); process hosts ship without snapshots
