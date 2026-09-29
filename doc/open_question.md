# Open Questions

Every unresolved design question and undecided value, in one place. Design docs link here
instead of discussing open points inline.

Each entry says what is unresolved, where the docs touch it, and which milestone it blocks
(milestones are listed in `glossary.md`). When a question is answered, write the answer into
the design doc and delete the entry. IDs are never reused.

Doc names are shortened as in `glossary.md` (`engine_core` is `design_engine_core.md`, and
so on).

---

## Blocks M1

### Q1. How is the host paced?

`engine_core` §3.3 treats the host as a run grant (`run_until`): in single-player the gate
never waits for it. `python_api` §4.1 and §7.1 say the host must call `declare_ready(t)` for
every tick, or the gate never opens.

Both choices have a cost. If the host must declare ready, one declare releases one tick, so
catch-up and fast-forward stop working, and the headless and replay loops (`python_api`
§4.2) never declare at all. If the host is not paced, a host command in sim-thread mode can
miss its tick's drain and cause a protocol error, which breaks "every producer is paced"
(`engine_core` §5.1).

### Q2. Must every mod be paced every tick?

`modding` §4.1 and §4.2 make every mod a participant that declares ready every tick, even
one that never submits. This conflicts with "no Python callbacks at tick rate" (`python_api`
§0, §5) and with "reading does not make you a participant" (`engine_core` §3.3). As written,
the slowest mod's Python loop limits the tick rate. Options: read-only mods become
observers; a mod declares ready for a range of ticks at once. Also blocks M7.

### Q3. Can `engine.snapshot()` be called from any thread?

`python_api` §6 and §7.2, and `modding` §4.1, say yes. But it copies from the default View,
which is `PRIVATE`: one reader only (`engine_core` §3.1, §3.2). A second thread either races
the reader's take or copies a block the publisher is about to overwrite. Allowing it needs a
lock or pin on the reader side, which is a new cross-thread mechanism missing from the
mechanism register (`engine_core` §1.1).

### Q4. Can the default View afford to publish everything every tick?

The default View projects every `[[=viz]]` column of every row, every tick (`engine_core`
§3.1). At 10⁷ rows that is about 320 MB per publish, or 9.6 GB/s at 30 Hz (`limits` §5), and
the cost is paid even when nobody calls `snapshot()`. Options: make the default View
opt-out, lazy, less frequent or filtered. Also blocks M5.

### Q5. Is the `id` column in every projection?

`python_api` §7.2 and `data_container` §5.1 say yes. `engine_core` §3.4 says an unfiltered
View need not carry it, and `python_api` §7.2 also offers `identity=False`. If `id` can be
left out, it must become one of the declarations closed at the freeze (`engine_core` §2.4),
and snapshot lookups by id cannot work on such a View. Also blocks M5.

### Q6. How are the wake-up handshakes tested?

The docs (`engine_core` §3.2, §5.1) and `CLAUDE.md` say a mis-ordered variant of either
protocol fails only under ThreadSanitizer. That holds for data handed between threads. It
does not hold for the `seq_cst` wake-up handshakes in the gate (§3.3), the admission wait
(§5.1) and the backlog wake (§5.2). A lost wake-up is a hang, not a data race, so
ThreadSanitizer does not report it. These need another method, such as a model checker or
stress tests that detect timeouts.

### Q7. Does a caller ever see `session_paused`?

`engine_core` §3.3 and `python_api` §7.1 say a paused session wakes waiting submitters with
`session_paused`, so a pause is not mistaken for a hang. But the submit pseudo-code in
`engine_core` §5.1 retries internally, and `session_paused` appears in no result list.
Either add it as a result everywhere or drop the wake-up.

### Q8. Can the admission wait ever happen?

A ring's depth is `(margin + 1) × capacity`, each tick holds at most `capacity` commands,
stamps stay within the margin, and the drain empties every earlier tick first (`engine_core`
§5.1). Together these seem to guarantee there is always room, so the admission wait, its
wake-ups and its tests may be dead code. Either name the case that reaches it or remove it.
Related: Q15.

### Q9. How does a stopped participant leave the gate?

Nothing removes a participant when its mod is quarantined, fails to spawn, or is stopped by
`mods.stop()` while the session keeps running (`modding` §4.2, §6; `python_api` §2). Only
the expiry policy removes one, after a full deadline stall each time (`engine_core` §3.3).
Also: no manifest or config field declares a mod's deadline or expiry policy, and the memory
order of the `SUSPEND` write is unspecified. Also blocks M7.

### Q10. Can the event ring fill within one tick?

The event ring is sized `C × D`, which assumes events are bounded by commands (`engine_core`
§5; `python_api` §7.1). Events the engine creates itself are not: cap rejections,
participant changes, suspensions, failures. The backlog is only checked at the gate, so the
ring could fill during a tick, and no doc says what happens then. `high_water` needs
headroom for one tick, and that headroom is not bounded. Also unclear: which thread enqueues
"mod suspended", since the ring has one producer.

### Q11. How does draining events wake the sim?

The event backlog is a plain count the sim computes as `write − read` (`engine_core` §5.2).
For a drain to wake a sim parked on the backlog, the owner's store of `read` must be
`seq_cst` followed by a signal to a parked sim, and the sim must recompute the backlog
inside the gate loop, after arming. §5.2 requires the wake-up but does not give the store
order of `read`, and its pseudo-code recomputes the backlog "before the gate". The mechanism
register (§1.1) has no row for it.

### Q12. Does `step()` publish every View regardless of cadence?

`python_api` §4.3 and §7.2 say a stepped tick publishes every View. `engine_core` §3.1 and
`limits` §1 publish only Views that are due by their cadence.

### Q13. Can `close()` be called from another thread?

`python_api` §2 says `close()` never raises. The same section, and §8, say it is an
owner-thread call that raises `EngineThreadError` elsewhere.

### Q14. How does a paced View count as ready?

A View registered with `paced=True` holds the core back until it is taken (`python_api`
§7.2). Neither `engine_core` §3.2 nor §3.3 says how a take counts as readiness, or how that
combines with a cadence above 1.

### Q15. Simplify the gate and the endpoint lease?

The v1 simplification pass left three parts unrevised: the progress vocabulary of the
mechanism register (`engine_core` §1.1), the gate's `seq_cst` arming (§3.3), and the
endpoint lease (§5.1). Whether they survive the same simplicity argument is open. Related:
Q8.

### Q16. Which events exist?

The event list is not designed (`python_api` §7.3). M1 needs at least session, integrity and
participant events.

### Q17. Should resource failures have their own terminal state?

GPU device loss and allocation failure during growth currently end in `failed`, which
poisons the process (`python_api` §2). Whether they deserve a cleaner terminal state is
open.

### Q18. What goes in `WindowConfig` and `LogConfig`?

Both are named in `EngineConfig` without fields (`python_api` §3; `design_logging.md` §3).

### Q19. Is the Python logging refcount thread-safe?

The logging wrapper is reference-counted (`design_logging.md` §2), but the free-threaded
build has no stated synchronization for that count (`python_api` §6).

### Q20. Where does View block growth go in the tick budget?

Publish may grow a View's block pool (`engine_core` §3.1), but growth has no line in the
tick budget, and nothing triggers growth in pool count.

### Q21. What are the catch-up clamp and `IDLE_SLICE`?

Both are named (`python_api` §4.3; `modding` §4.2) with no value anywhere. Also blocks M6.

### Q22. How large is one command or event slot?

No doc gives a slot size, so worst-case ring memory is a count, not bytes (`limits` §2.2).
Depends on the payload schema (`engine_core` §2.3).

### Q62. In what order are two endpoints of one source drained?

A source with several producer threads takes one endpoint per thread (`engine_core` §5;
`python_api` §7.1; `modding` §4.1). The drain visits endpoints in ascending source id
(`engine_core` §5.1), which does not order two endpoints of the same source.

### Q63. What is `high_water`?

The event backlog size at which the gate pauses the sim (`engine_core` §5.2). The counter,
its owner and its place in the gate are decided; the value needs a measured drain rate
(`limits` §7). It must leave headroom for one tick of events (Q10).

### Q65. Which `failed` paths does `on_failed_stop` cover?

`python_api` §3 applies `on_failed_stop` to the sim-thread join timeout and an abandoned
thread host, and §4.3 applies it to `step()`'s second expiry. `failed` has more entry paths
(`python_api` §2): the host's gate deadline under `FAIL` (`engine_core` §3.3), an endpoint
revocation timeout (`engine_core` §5.1, `modding` §6), the operation-lease wait timeout in
`close()`, GPU device loss (`engine_core` §5) and growth allocation failure
(`data_container` §2.2). No doc says whether `"terminate"` aborts on these, or what
`"raise"` means where no call can raise. The `step()` expiry and the host's `FAIL` expiry
may also be the same path.

## Blocks M2

### Q23. What does "engine build" mean in session identity?

`engine_core` §2.3 puts the engine build in the replay header, and a mismatch rejects a
replay (§2.4, `python_api` §4.2). But the same section requires identical checksums between
Debug and Release builds, certification runs both (`modding` §5.3), and golden replays are
meant to work across revisions. A build hash would reject all of these; a determinism
version number would not.

### Q24. Are per-system checksums required?

`modding` §5.3 needs them to blame a desync on a mod's system. `engine_core` §2.3 makes them
optional, and `python_api` only exposes `engine.checksum()`.

### Q25. The `fixed<>` specification

Library choice, and the bit-level rules for rounding, division, division by zero and
overflow (`engine_core` §2.1). Q32.32 on `int64` is the working assumption.

### Q26. The PRNG

Which generator (PCG or xoshiro) and how streams are derived (`engine_core` §2.2; `limits`
§7).

### Q27. The checksum's exact algorithm and inputs

The family is decided: a 64-bit, id-mixed, order-independent reduction (`limits` §6). Still
open: the bit-level algorithm (`engine_core` §4.2), and a list of exactly which state can
affect a future tick and so must be hashed (§2.3).

### Q28. Tick counter width and wrap

The tick counter is implicitly `u64` everywhere, but no doc states its width or what happens
on wrap.

### Q29. How are bitfields made canonical?

A `bitfield` column's unused trailing bits could differ between machines and cause false
desyncs. Its snapshot type and NumPy dtype are also undefined (`data_container` §2.1, §3,
§7.3).

### Q30. Is `std::simd` enough?

`std::simd` is the chosen SIMD layer (`data_container` §3), with Highway as fallback. It is
unproven for fixed-point high-multiply and for gather/scatter (`engine_core` §4.4).

## Blocks M3 to M5

### Q31. What does hitting a declared cap do?

A cap is either a game rule (reject deterministically) or a ceiling (stop and report, the
default) (`data_container` §2.2). Neither `[[=capacity(N)]]` nor `EngineConfig.capacity` can
say which kind. What "stop" means is undefined: pause or `failed`? For a Tier 3 mod's
system, "suspend that mod" is defined only for Tier 2 mods. M3.

### Q32. Relationships

Link storage, indexing, composite keys and delete-time fix-up are undesigned
(`data_container` §1.4, §2.2). Start with unique links and many-links as arrays. M3.

### Q33. The parallel compaction gather

Order-preserving compaction moves rows down, so in-place parallel workers can overwrite rows
another worker has not read yet (`engine_core` §4.1; `data_container` §2.2, §4). The gather
must be out-of-place (extra memory) or split into steps. The row predicate reuses this pass
(`engine_core` §3.4). M3.

### Q34. Windows and C++26

Conan limits MSVC to C++23. Options: GCC on Windows, clang with P2996, or hand-written
containers (`data_container` §8; `limits` §7). Before M3.

### Q35. Where is the predicate scan charged?

On the sim thread or the worker pool (`data_container` §5; `limits` §5, §7). The answer
shapes the phase structure. Before M3.

### Q36. Where are per-(type, mod) attribution counts reported?

Named in `limits` §4 and `data_container` §7.3, with no surface, cadence or reader. M3.

### Q37. How do systems in one phase run?

A phase may contain a `write_local` writer and another system's `read_local` on the same
column (`engine_core` §4.1). If those systems run concurrently on the same rows, they race.
The execution order inside a phase is unspecified. M4.

### Q38. What happens when a per-worker log buffer fills during a phase?

`modding` §5.2 and `design_patterns.md` §7 only say volume is bounded by the buffer. M4.

### Q39. The GPU allocator for growing worlds

How GPU buffers grow for uncapped `[[=gpu]]` types (`data_container` §5, §7.4). The one
fixed constraint: each upload rewrites a whole buffer. Before M3, used in M5.

### Q40. Naming the publisher's write path

The snapshot container has no mutating API (`data_container` §5.1), yet the publisher writes
it. That write path needs a defined, named interface. M5.

### Q41. Dynamic `viz` columns in snapshots

The shape of the type-erased part of a generated snapshot holding mod-added `viz` columns,
and its NumPy dtype. M5.

### Q42. The warning thresholds

The projection warning (4 GB/s per View) is reasoned, not measured; M5 owes a profile. The
compaction warning (1 GB/s per object type) has neither measurement nor derivation, and at
10⁶ rows of 32 bytes erased every tick it would already be close; M3 owes a profile
(`limits` §4, §5, §9).

### Q61. What row width does the predicate scan assume?

`engine_core` §3.1 and `limits` §5 cost a position test at 12 bytes per row. Positions are
Q32.32 on `int64` (`data_container` §3), which is 16 bytes in 2D or 24 in 3D, and the
world's dimensionality is not stated. The scan cost at 10⁷ rows (`limits` §7) needs
recomputing once this is settled. M5.

## Blocks M6 and M7

### Q43. `ipc_deadline`: value and constraints

It must exceed one tick (`limits` §1.1), and its ceiling is said to be the input delay
(`limits` §7; `modding` §4.3). With an input delay of 1 tick that leaves no valid value, and
single-player has no input delay at all. `modding` §4.3 also uses a 10 ms example. Nothing
explains why a local process deadline should follow a network margin. M7.

### Q44. What `commands_per_tick` in a manifest means

The manifest example requests 4096 while the endpoint cap is 256 (`modding` §3; `limits`
§7). Decide whether it is per-endpoint capacity or something else, then rename it. Before
M2.

### Q45. Protecting reliable events in mod inboxes

"Reserve capacity or a sticky loss counter" (`python_api` §7.3; `engine_core` §5), while
`modding` §4.2 uses both. Also unclear: when an overflow suspends the mod rather than
dropping or merging events by delivery class. M7.

### Q46. Process-host drain timeout

`modding` §6 says to kill the process and confirm through its supervisor, but the supervisor
is an engine-side thread (`modding` §4.3) and cannot be killed. `engine_core` §5.1 says an
endpoint-lease timeout enters `failed`. M7.

### Q47. Unloading a suspended mod

Needs the endpoint reclaimed while admitted commands remain, and a source id retired even
though it is in the replay header (`modding` §4.2; `engine_core` §5.2). Deferred.

### Q66. How does a mod declare an endpoint per producer thread?

A source with several producer threads takes one endpoint per thread (`engine_core` §5.1;
`modding` §4.1), but the manifest has no field to declare them (`modding` §3). How two
endpoints of one source are ordered in the drain is Q62. M7.

### Q64. Bounding a process host's copy-on-write pages

On Windows, `FILE_MAP_COPY` is reachable from a `FILE_MAP_READ` handle, so a process-host
child can privately write its read-only View payload (`modding` §4.3, platform constraints).
Engine memory stays safe, but the dirtied private pages are charged to the paging file, and
nothing bounds that. A commit limit on the child's Job object is one candidate. Deferred
with process-host Views (`engine_core` Appendix A); after M7.

## Multiplayer (after M7)

### Q48. Stall policy

Timeouts, drops, and what a dropped peer's absence means for the command stream. The shape
is fixed (`multiplayer` §4.1): only the server tier decides, and a drop applies at an agreed
turn. The details need a transport.

### Q49. Peer deadline and `DROP` escalation

The deadline value for peer participants needs a measured round-trip time. The escalation
protocol behind `on_expiry = DROP` is undesigned (`engine_core` §3.3; `multiplayer` §4.1).

### Q50. `input_delay_ticks`

The value of a peer's stamp margin (`multiplayer` §3.2). It needs measurement against real
round-trip times, and the transport owns it. Ring depth, the `ipc_deadline` ceiling (Q43)
and the worst-case totals in `limits` §2.2 depend on it. Blocks nothing in M1.

### Q51. Transport and session formation

Transport, session formation, NAT traversal, rejoin and late join. Nothing in the core
depends on the choice. Late join needs a bit-exact state transfer, which is not the snapshot
format (`engine_core` §5).

### Q52. Checksum comparison between peers

How often peers compare checksums, and what a detected desync does. The checksum itself is
decided (`limits` §6).

### Q53. Non-deterministic peripherals in multiplayer

Elect one peer to compute and broadcast their commands, or require the mind to run inside
the determinism boundary; per subsystem or globally (`multiplayer` §4; `engine_core` §2,
§6). This affects `engine_core` §6, not the command path.

### Q54. Enforcing the same mod set on every peer

Session identity records the mod set but peers never compare it. This is a check during
session formation, not an engine mechanism.

### Q55. The local player's source id

On each machine the local player submits as the host (source 0, margin 0; `python_api`
§7.1). On other machines the same player's commands arrive on that player's peer endpoint,
with a different source id. Unless the local player also submits through its peer endpoint
with the input-delay margin, machines drain in different orders (`multiplayer` §3.2, §4).

## Not tied to a milestone

### Q56. Keep or drop flecs

`engine_core` §4.5; `data_container` §6.

### Q57. GPU per-entity state across frames

Trails, selection and level-of-detail caches need a GPU-visible id column or a CPU-side
id-to-row rebuild; neither exists (`data_container` §5).

### Q58. Mod link traversal

Letting mods follow relationship links needs a way to describe another type's row set
(`data_container` §7.3). Out of scope for v1.

### Q59. Mod-added column data when a mod is removed from a save

Resolve when save/load serialization tags are designed (`data_container` §8).

### Q60. Reviving the `SHARED` View

If process hosts need snapshots, choose between a per-reader epoch slot and the original pin
count (`engine_core` Appendix A; `limits` §9).
