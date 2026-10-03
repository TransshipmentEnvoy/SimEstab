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

No open question blocks M1.

## Blocks M2

### Q22. How large is one command or event slot?

A ring slot holds one command or event payload, and no doc gives its size, so worst-case
ring memory is a count, not bytes (`limits` §2.2, §7). M1 uses a provisional constant. The
real rule depends on the payload schema (`engine_core` §2.3), which M2 specifies.

### Q23. What does "engine build" mean in session identity?

`engine_core` §2.3 puts the engine build in the replay header, and a mismatch rejects a
replay (§2.4, `python_api` §4.2). But the same section requires identical checksums between
Debug and Release builds, certification runs both (`modding` §5.3), and golden replays are
meant to work across revisions. A build hash would reject all of these; a determinism
version number would not.

### Q25. The `fixed<>` specification

Library choice, and the bit-level rules for rounding, division, division by zero and
overflow (`engine_core` §2.1). Q32.32 on `int64` is the working assumption.

### Q26. The PRNG

Which generator (PCG or xoshiro) and how streams are derived (`engine_core` §2.2; `limits`
§7).

### Q27. The checksum's exact algorithm and inputs

The family is decided: a 64-bit, id-mixed, order-independent reduction (`limits` §6). The
inputs are decided in outline: the columns of live slots, and the generation column, the
live and retired bitmaps and the extent of every object type (`engine_core` §2.3;
`data_container` §2.2). Derived columns are left out of the tick digest and the rolling
checksum (`data_container` §2.1). Still open: the bit-level algorithm (`engine_core` §4.2),
and a complete list of the other state that can affect a future tick and so must be hashed.

### Q67. The rolling checksum period and the tick digest

The rolling checksum hashes the chunks whose index is congruent to the tick modulo N, so it
covers the world every N ticks (`engine_core` §2.3; `limits` §6). N is proposed as 30, one
second at 30 Hz, but a value in `limits` must also land in code. Also open: exactly what the
tick digest hashes beyond the drained commands and each type's live count and extent, and
whether the rolling checksum runs in single-player while a replay is being recorded.

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

Every object type has a cap, 2²⁴ unless declared (`data_container` §2.1). A cap is either a
game rule (reject deterministically) or a ceiling (stop and report, the default)
(`data_container` §2.2). The default cap is a ceiling. Neither `[[=cap(N)]]` nor
`EngineConfig.entity_capacity` can say which kind. What "stop" means is undefined: pause or
`failed`? For a Tier 3 mod's system, "suspend that mod" is defined only for Tier 2 mods. M3.

### Q32. Relationships

Link storage, indexing, composite keys and delete-time fix-up are undesigned
(`data_container` §1.4, §2.2). Start with unique links and many-links as arrays. M3.

The pool fixes four constraints (`data_container` §2.2):

- A link stores the target's slot. Its width follows the target type's cap: `u16` up to
  2¹⁶−1, `u32` above. How a null link is encoded is open with the rest.
- An erase fixes every link into the erased entity in the terminal commit, by the
  relationship's delete rule, before any create can reuse the slot.
- Links to entities that survive never change, because rows never move.
- A reverse index (the links into one entity) is kept in slot order, or rebuilt in slot
  order, so its order follows from state.

dcon offers candidates to evaluate:

- `vector_pool` storage for the reverse lists of many-links;
- primary-key relationships, stored in the slots of the object they belong to;
- composite keys backed by a hash map that is used only for lookups, never iterated;
- `multiple` links, which hold several targets in one link;
- delete rules: cascade (delete the relationship) or set null (dcon's `optional` links).

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

Every object type has a cap, but sizing GPU buffers at the cap would reserve device memory
nobody uses. So each GPU destination, and each engine view block, is sized from its engine view's maximum
rows (`data_container` §5, §5.1, §7.4). Open: how an engine view declares its maximum, what the
default is, and whether destinations suballocate from a few large buffers. The one fixed
constraint: each upload rewrites a whole buffer. Before M3, used in M5.

### Q40. Naming the publisher's write path

The snapshot container has no mutating API (`data_container` §5.1), yet the publisher writes
it. That write path needs a defined, named interface. M5.

### Q41. Dynamic `viz` columns in snapshots

The shape of the type-erased part of a generated snapshot holding mod-added `viz` columns,
and its NumPy dtype. M5.

### Q42. The warning thresholds

The projection warning (4 GB/s per engine view) is reasoned, not measured; M5 owes a profile
(`limits` §5, §9).

### Q61. What row width does the predicate scan assume?

`engine_core` §3.1 and `limits` §5 cost a position test at 12 bytes per row. Positions are
Q32.32 on `int64` (`data_container` §3), which is 16 bytes in 2D or 24 in 3D, and the
world's dimensionality is not stated. The scan cost at 10⁷ rows (`limits` §7) needs
recomputing once this is settled. M5.

### Q68. Per-index column groups

dcon's `array{index}{T}` property holds one value per row of a fixed index type, stored as
one column per index value, for example `pop.demand[commodity]` (`data_container` §1.4). An
establishment economy needs this shape. The index type's row count must close at the
freeze. Open: how the group is declared, how its columns get column ids and catalog names,
how the mod ABI resolves them, and how checksum and serialization cover them. M3.

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

A source has one endpoint in v1, so a mod with several producer threads serializes them
(`engine_core` §5.1; `modding` §4.1). Whether a mod may declare one endpoint per thread, and
the manifest field for it (`modding` §3), are open. How two endpoints of one source are
ordered in the drain is Q62. M7.

### Q62. In what order are two endpoints of one source drained?

A source has one endpoint in v1, and a source with several producer threads serializes its
own submissions (`engine_core` §5.1; `python_api` §7.1; `modding` §4.1). If a source ever
gets one endpoint per thread (Q66), the drain needs an order between them: it visits
endpoints in ascending source id (`engine_core` §5.1), which does not order two endpoints of
the same source. M7.

### Q64. Bounding a process host's copy-on-write pages

On Windows, `FILE_MAP_COPY` is reachable from a `FILE_MAP_READ` handle, so a process-host
child can privately write its read-only engine view payload (`modding` §4.3, platform constraints).
Engine memory stays safe, but the dirtied private pages are charged to the paging file, and
nothing bounds that. A commit limit on the child's Job object is one candidate. Deferred
with process-host engine views (`engine_core` Appendix A); after M7.

### Q80. The mod gate deadline: default and cap

A mod with a paced engine view declares `deadline_ms` in its manifest.
`HostPolicy.mod_deadline_ms` applies when it declares none, and
`HostPolicy.max_mod_deadline_ms` caps it (`modding` §3; `python_api` §3; `limits` §1.1).
Neither value is decided. The deadline is how long one slow mod's take may hold a tick
before the gate continues without its view, so it trades tick latency against how often a
busy mod's view drops out. It needs a measured mod-loop time. M7.

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
round-trip times, and the transport owns it. The `ipc_deadline` ceiling (Q43) depends on it;
no ring depth does. Blocks nothing in M1.

### Q51. Transport and session formation

Transport, session formation, NAT traversal, rejoin and late join. Nothing in the core
depends on the choice. Late join needs a bit-exact state transfer, which is not the snapshot
format (`engine_core` §5).

### Q52. Checksum comparison between peers

Peers compare the tick digest every tick and the rolling checksum as it completes each
slice (`engine_core` §2.3; `limits` §6). Open: what a detected desync does, and whether a
suspected desync triggers a full checksum.

### Q53. Non-deterministic peripherals in multiplayer

A logic mod runs once, on a mod client, and its commands travel in the turn stream
(`multiplayer` §4.2). Open: how session formation assigns logic mods to mod clients; what
happens when a mod client drops (restart the mod elsewhere, or let its source go quiet);
the stamp margin a mod client declares, which must cover its failover sites; and whether a
player's own bot may ride in that player's turns, which needs the server tier to accept a
second source id from that client. For other minds outside the boundary: elect one peer to
compute and broadcast their commands, or require the mind to run inside the determinism
boundary, per subsystem or globally (`multiplayer` §4; `engine_core` §2, §6).

### Q54. Enforcing the same mod set on every peer

Session identity records the mod set but peers never compare it. This is a check during
session formation, not an engine mechanism.

### Q55. The local player's source id

In a networked session the local player's tick-less submits go to the turn assembler, which
stamps them and sends them out. Every machine receives them through that player's peer
endpoint, produced by the receive thread, so they carry the same source id and tick
everywhere (`multiplayer` §3.2). A logic mod's commands take the same route. Two things are
open. First, whether the host endpoint itself is closed in a networked session, as it is in
playback (`python_api` §4.2). Second, the turn assembler's protocol: how it learns the next
open turn, and how it reports `queue_full` and outcomes back to the source. Only the server
tier may decide that a late player has dropped (`multiplayer` §4.1).

## Not tied to a milestone

### Q56. Keep or drop flecs

`engine_core` §4.5; `data_container` §6.

### Q57. GPU per-entity state across frames

Trails, selection and level-of-detail caches need per-entity state that survives from one
frame to the next. Snapshot rows shift, but core slots are stable for an entity's whole
life, and both halves of an id are `u32` (`data_container` §2.2, §5). So such state could
key on slot and generation, if an engine view projects them. Whether and how engine views project them is
open.

### Q58. Mod link traversal

Letting mods follow relationship links needs a way to describe another type's row set
(`data_container` §7.3). Out of scope for v1.

### Q59. Mod-added column data when a mod is removed from a save

Resolve when save/load serialization tags are designed (`data_container` §8).

### Q60. Reviving the `SHARED` engine view

If process hosts need snapshots, choose between a per-reader epoch slot and the original pin
count (`engine_core` Appendix A; `limits` §9). A `SHARED` engine view may also grow its block pool
in number of blocks (`engine_core` §3.1). That growth has no line in the tick budget, and
nothing yet triggers it.

### Q69. A dense storage kind

Every object type is a pool, with holes where entities were erased (`data_container` §2.2).
A type whose population shrinks can leave many dead lanes in its scans. dcon's `compactable`
storage, behind a sparse id-to-row table, would keep such a type dense. It costs a second
lookup path and link patching on every move, and it must keep the rule that only full ids
cross a boundary. Revisit when a profile shows scan time lost to dead lanes
(`data_container` §1.4).

### Q70. fmt or `std::format`

fmt is a dependency that no code uses, and it is linked `PUBLIC`, so every consumer must
provide it. Logging formats through Boost.Log streams. GCC 16 has `std::format` and
`std::print`. Compare build time in module builds, the formatting features the code will
need, and MSVC support (Q34).

### Q71. range-v3 or `std::ranges`

range-v3 is a dependency that no code uses, and it is linked `PUBLIC`. `std::ranges` in
GCC 16 includes `views::concat` and `views::enumerate`. range-v3 still has actions (eager
algorithms on containers), which have no standard counterpart. Its last release is from
2022.

### Q72. tl-function-ref or `std::function_ref`

tl-function-ref is a dependency that no code uses. GCC 16 has `std::function_ref`. Check
MSVC support before relying on the standard one (Q34).

### Q73. magic_enum or reflection for enum names

magic_enum is a dependency that no code uses. C++26 reflection lists enumerators directly
(`std::meta::enumerators_of`), and the data container already relies on reflection
(`data_container` §2). magic_enum only sees values in a fixed range (-128 to 127 by default)
but also works on MSVC. Reflection has no such limit but needs GCC 16 (Q34).

### Q74. scope-lite or Boost.Scope

Every submit enters the endpoint lease, and a scope guard leaves it on every exit
(`engine_core` §5.1). scope-lite and Boost.Scope both provide `scope_exit`, `scope_fail`,
`scope_success` and `unique_resource`. Boost.Scope ships with the Boost already in the
build. scope-lite is a dependency that no code uses.

### Q75. tsl-robin-map or `boost::unordered_flat_map`

tsl-robin-map is a dependency that no code uses, and it is linked `PUBLIC`.
`boost::unordered_flat_map` ships with the Boost already in the build. Both are
open-addressing maps without pointer stability. Compare them on the planned uses: lookups
only, since state changes never come from iterating a hash map (`engine_core` §2.2), for
example composite keys (Q32).

### Q76. Keep indicators

indicators draws progress bars in a terminal. It is a dependency that no code uses, and no
doc plans a use for it.

### Q77. boost-ext-ut or Boost.Test

The C++ tests use Boost.Test through CTest. boost-ext-ut is a dependency that no code uses.
It is worth keeping only if the tests move to it.

### Q78. Audio drivers on Linux

The bundled SDL recipe builds one Linux audio driver, PipeWire. SDL loads
`libpipewire-0.3.so.0` at run time. Without a running PipeWire server, the audio subsystem
fails to start. That covers systems with only PulseAudio or only ALSA, and most containers.
SDL's dummy and disk drivers never start on their own; only `SDL_AUDIO_DRIVER` selects
them. Windows and macOS use their own audio APIs and are not affected.

Nothing initializes SDL audio yet, so decide before the first use. The `alsa` option is the
smallest fallback: it adds `libalsa`, and SDL tries ALSA when PipeWire fails. Check that a
Conan-built `libalsa` finds the system's ALSA configuration and plugins. The `pulseaudio`
option adds the `pulseaudio` package instead. Both are SDL options in
`src/libsim_estab/conandata.yml`.
