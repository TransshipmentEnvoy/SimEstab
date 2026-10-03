# Engine Limits: Decided Values

This document gives every number that the other design docs leave to measurement or to a
later decision. For each value it gives the value, where it lives in code, the reasoning,
and what would make it change (§9).

Status: M0 is done: every decided value below is in `sim_estab:limits` or `config.py`.

Terms are defined in [glossary.md](glossary.md). Five sites send readers here for their
values: `design_engine_core.md` §3.1, `design_python_api.md` §3 and §7.1, and
`design_data_container.md` §2.2 and §4. Those sites describe the mechanism; the number is
here.

**This document holds the reasoning; the code holds the value.** A number that appears here
but nowhere in the build is not decided. The tick budget of `design_engine_core.md` §3.1
needs numbers that some step of its build order (§7 there) owns, and this document decides
them. §8 names the one place each value lives, and **the two must not drift**.

Two kinds of number live here, and they behave differently:

- **Build constants** are fixed when the library compiles. Changing one is a rebuild, never
  a config change. Examples: the chunk quantum (§3), the caps of §2 and the object caps of
  §4.
- **Policy defaults** are the shipped defaults of fields a session may override at
  construction, in `EngineConfig`. Examples: the tick rate, `D`, endpoint capacity. The
  engine reads the configured value, never the default. The defaults exist so that C++ and
  Python cannot drift, and so that a session that configures nothing still has a value.

A third kind, a cap on what one pass may cost, is left empty on purpose. Projection (§5) is
measured instead, against a warning threshold. The rows any pass can touch are bounded by the
object caps of §4.

---

## 1. Tick rate: 30 Hz

The tick rate fixes how much simulated time one tick covers, and so the length of the tick
budget. The default is 30 Hz. The rate is part of the replay header and is not a speed
control.

`tick_rate = 30`, a policy default, in `EngineConfig.tick_rate`.

**30 Hz ticks under a 60 fps render is the normal case** (`design_python_api.md` §4.1,
`design_data_container.md` §5). Rate-dependent figures in this document use 30 Hz, including
the 33.3 ms tick, the span of `D` (§2), the command rate (§2.2), the publish bandwidths (§5)
and the scan estimate (§7). The upload example in `design_data_container.md` §5 assumes 60
uploads per second. Uploads normally follow the tick rate, but a tick can be uploaded again
while its fence is pending, so at 60 fps the example is a real worst case.

One **tick** is one engine step. It passes the gate and drains each endpoint's ring in
order. It runs the system list through its phases, applies every structural change in the
terminal commit, and publishes every due engine view. Publishing always happens. An engine view always has
a writable block, so a due publish is never skipped (`design_engine_core.md` §3.1).
`step(n)` advances exactly `n` ticks, and the tick counter is the simulation clock. At 30 Hz
a tick is 33.3 ms of simulated time. At normal speed, 33.3 ms is also the wall-clock tick
budget of `design_engine_core.md` §3.1. Time spent waiting at the gate is pacing and is not
charged.

**Why 30 and not 60.** Every charge in the tick budget is paid once per tick: publish, the
terminal commit, the checksum, the command drain and system execution. So the rate
multiplies all of them equally. 60 Hz would buy finer input timing, which matters
less than the saving on every row of the tick budget.

**The rate is not a speed knob.** This is restated here because this is where a reader would
look for one. Changing the rate changes what a tick means, so it changes outcomes, not
pacing. The rate is frozen at construction and recorded in the replay header. A replay
recorded at one rate is rejected by the header at another. Game speed is the time scale, set
by `set_time_scale`, a control call outside determinism.

**Nothing rescales when the rate changes.** `D` (§2) is a count of ticks, so its wall-clock
span follows the rate: 267 ms at 30 Hz, half that at 60 Hz. The `HostPolicy` deadlines
(§1.1) are in seconds, so they keep their wall-clock length and cover a different number of
ticks. `EngineConfig` checks a set `ipc_deadline` against the configured rate. The reasoning
for `D` and `ipc_deadline` assumes 30 Hz, so a session at another rate should re-check them.

### 1.1 Host policy: what the engine enforces on mod hosts

These values are policy defaults in `EngineConfig.host_policy`. Other docs name these fields
and leave their values to this document.

| Field | Value | Reasoning |
|---|---|---|
| `shutdown_deadline` | **5.0 s** | Seconds. Each blocking shutdown stage gets the full value for itself. There are five stages. The first waits for running submits when an endpoint is revoked. The second is the mod host's inbox drain and `on_unload`. The third joins the mod host and the sim thread. The fourth confirms that a killed process is dead, and the fifth waits for running engine calls in `close()`. One value keeps the escalation policy consistent. It is a limit per stage, not one total deadline. On expiry the engine kills a process where it can. Otherwise it enters `failed` and disarms, rather than reclaiming memory that may still be in use. |
| `ipc_deadline` | **open on purpose** ([Q43](open_question.md#q43-ipc_deadline-value-and-constraints)); its ceiling is the input delay | It is not a free parameter, so no number stands here. Its ceiling is the input delay: the window of ticks a peer's command may legitimately be in flight for (`design_multiplayer.md` §3.2). Inside that window nothing is wrong; past it, something is. It must also exceed one tick. A deadline under 33.3 ms can only detect that the other side is mid-tick, which is not a fault. It would also expire on any hot-path round trip that crosses a tick boundary. So its value waits for `input_delay_ticks` (§7). It bounds the **hot-path round trip only**. A process mod's wait for an outcome is a separate, cancellable operation with no deadline of this kind. A wait that may legitimately span a tick cannot share a bound with one that must not (`design_modding.md` §4.3). |
| `mod_retry_limit` | **0** | A failed mod stays disabled: quarantine restarts nothing by default. This is the default the other docs already give, recorded here so the set is complete. |
| `max_inbox_size` | **1024** | The engine ceiling on the `inbox_size` a manifest requests (`design_modding.md` §4.2). The example manifest requests 256 (`design_modding.md` §3). Each mod declares its own inbox size, so without a cap total inbox memory would grow with the number of installed mods, not with anything the engine chose. 1024 is four times the example request. It does not bind a normal mod and still bounds a pathological manifest. |
| `mod_deadline_ms`, `max_mod_deadline_ms` | **open on purpose** ([Q80](open_question.md#q80-the-mod-gate-deadline-default-and-cap)) | A mod's gate deadline: the value when its manifest declares no `deadline_ms`, and the engine's cap on one it does declare (`design_modding.md` §3). It is how long one slow mod may hold a tick before it is suspended, so it trades tick latency against how often a busy mod is suspended. That needs a measured mod-loop time, which nothing has yet. |

## 2. Command endpoints, the three counts, and the engine view cap

This section sets the endpoint capacities and `D`, and derives the per-tick totals from
them. §2.1 separates players, sources and participants. §2.2 gives the worst case, and §2.3
caps the number of engine views.

The capacities and `D` are policy defaults in `EngineConfig.command_policy`
(`design_python_api.md` §7.1). The engine cap on capacity is a build constant.

**An endpoint declares one number: its capacity.** Nothing caps how many of its commands a
tick may run. Pacing closes the set before the drain runs (`design_engine_core.md` §5.1),
and the tick then runs all of it. Nothing is ever deferred to a later tick, so ring depth
needs no extra room for deferral. Depth follows from capacity and the endpoint's stamp
margin (below).

| Symbol | Name | Value | Reasoning |
|---|---|---|---|
| — | endpoint capacity, default | **64** | Commands one endpoint may hold for one tick. Mods declare it in their manifest; this is the value when nothing is declared. Exceeding it is `queue_full`, the only capacity rejection in the engine |
| — | endpoint capacity, engine cap | **256** | The ceiling a manifest may request |
| — | host endpoint (source 0) capacity | **256** | The host gets the ceiling by default. Its endpoint is unpaced, so the number counts the commands waiting for the next drain, which every drain empties (`design_engine_core.md` §5.1). It needs no reserve carved out of an engine-wide pool, because rings are per endpoint: no mod can use up the host's capacity, whatever it submits |
| — | peer endpoint capacity | **256** | A peer's endpoint carries a whole remote player's turn, so it gets the ceiling for the same reason as the host |
| `D` | maximum ticks between `drain_events()` calls | **8** | 267 ms at 30 Hz: about sixteen frames of slack at 60 fps for a host that drains every frame, and short enough to catch a host that stopped draining. A tolerance, not a promise |
| `high_water` | event backlog at which the gate pauses the sim | **derived**: `(C + E) × D` (below) | `D` drain intervals' worth of events. It is not a value to pick: the ring holds it plus one tick, so the tick that passes the gate just below it always fits (`design_engine_core.md` §5.2) |

**Ring depth is derived from capacity and the endpoint's stamp margin**, never declared:

| Endpoint | Depth |
|---|---|
| margin 0: engine sources, and mods by default | `capacity` |
| margin 1 or more: a peer, or a mod that declares a margin | `2 × capacity` |
| the host endpoint, unpaced | `capacity` |

A margin-0 producer acts inside the tick it is about to release, so it never has more than
one tick's worth outstanding. The host endpoint names no tick, and every drain empties it,
so it holds at most what the frame loop submits between two drains
(`design_engine_core.md` §5.1). An endpoint with a margin holds two ticks' worth: the tick
being filled and one already released. A producer further ahead waits for space until the
oldest tick in its ring has run, which cannot deadlock (`design_engine_core.md` §5.1). So
the depth does not depend on the margin's value, and a peer's input delay, which is open
on purpose ([Q50](open_question.md#q50-input_delay_ticks)), does not size any ring.

Five quantities are **derived** from these values and from counts the freeze closes. They
are computed, never restated. An implementation that hardcodes any of them has created a
second source of truth:

| Derived | Formula | Value at defaults | Where it binds |
|---|---|---|---|
| `C`: commands run per tick, engine-wide | `Σ capacity(endpoint)` | 256 + 64 × n_mod_endpoints | `design_engine_core.md` §5.1 |
| endpoint ring depth | `capacity` at margin 0 and on the host endpoint; `2 × capacity` at a margin of 1 or more | `capacity` for every margin-0 producer and the host; 512 for a peer | `design_engine_core.md` §5.1 |
| `E`: engine-created events per tick | `T·S + S + 3·P + V + 4`, over object types, sources, participants and engine views | `S + 3·P + V + 4` before cap refusals exist (M3) | `design_engine_core.md` §5.2 |
| `high_water` | `(C + E) × D` | `8 × (C + E)` | `design_engine_core.md` §5.2 |
| event ring size | `(C + E) × (D + 1)` | `9 × (C + E)` | `design_python_api.md` §7.1, §7.3 |

`C` is not a policy value. The drain assigns order by position and sorts nothing, so the
per-tick cost is `O(C)`. That makes `C` a result of the endpoint set, not a budget to divide
among it.

### 2.1 Three counts, two ceilings

Players, sources and participants are three different counts. Two of them nearly coincide,
so they are easy to confuse. **Each is defined where it is declared**, so that none can
stand in for another.

| Count | Value | What it counts |
|---|---|---|
| **Players** | **128** | How many humans a session admits. A game rule, not an engine structure: nothing in the engine is sized by it |
| **Sources** | **256** | Anything with its own command endpoint. Commands run in source id order, so every machine allocates one source per originating peer, plus the host, every mod that submits commands, and engine or AI sources |
| **Participants** | **256** | Anything the gate waits for, with a declared deadline and expiry policy (`design_engine_core.md` §3.3) |

**Why sources exceed players.** A full lobby is 128 originating peers before any mod
registers anything. A source ceiling equal to the player count would leave no room for mods
beside the players. So the two counts must differ, and 256 gives a full lobby 128 more
non-player sources.

**Why participants share that number.** Sources and participants differ in role, not in
size. Submitting makes a peripheral a participant, so the two sets nearly coincide. The
exception is a recorder, which is paced without submitting (`design_engine_core.md` §3.3).
One common ceiling means neither needs a smaller invented number, and scanning a 256-entry
participant array once per tick costs nothing. `sim_estab:limits` asserts that the
participant cap does not exceed the source cap. That holds because both caps are 256, not
because every participant is a source.

256 is a server node's worst case, not a client's normal one. On a 128-player client, the
participant set is its server node plus its local mods, a handful, because a client never
hears from peers directly (`design_multiplayer.md` §4.1).

**Raising a cap costs almost nothing.** `C = Σ capacity` over *registered* endpoints (§2),
so a cap is a bound checked at the freeze, not an allocation. A four-player session pays for
four sources, whatever the ceiling is. Only the worst case moves, which is why these
ceilings are set generously rather than tightly.

**Connections are not an engine cap.** Spectators, relays and server-tier links carry
commands without *originating* ordered ones. They cost sockets, not source ids. The
transport bounds them, and they have no row here.

### 2.2 The worst case, and what is still open inside it

This section gives the totals when every source registers at full capacity. They are
ceilings a session can reach, not memory every session allocates.

At 256 sources, each holding 256 commands for one tick:

| Quantity | Worst case | Note |
|---|---|---|
| `C`, commands per tick | **65,536** | ~2M/s at 30 Hz |
| command ring entries | **at most 131,072** | `Σ depth ≤ 2 × C`: a ring with a margin is two ticks deep |
| event ring entries | **599,652** before cap refusals; plus `9 × T × 256` from M3 | `(C + E) × (D + 1)`, with `E = 256 + 3 × 256 + 64 + 4 = 1,092` at 256 sources and participants and 64 engine views |
| total slot memory | **open** ([Q22](open_question.md#q22-how-large-is-one-command-or-event-slot)) | no doc gives a per-slot size, on purpose |

Both counts come from a `Σ` over what actually registered. A session reaches them only by
registering 256 full-capacity sources; a four-player game never pays for them. **Commands
per tick is a first-class metric**, in total and per source, for this reason
(`design_engine_core.md` §3.1). With no drain cap, a tick does all the work it is given. A
session nearing its ceiling should show as a rising number well before it shows as a
frame-time problem.

### 2.3 The engine view cap

**Two caps close the per-tick cost model.** Both are fixed at the freeze
(`design_engine_core.md` §2.4 step 3a).

| Cap | Value | Reasoning |
|---|---|---|
| Engine views per session | **64** | Publish cost is a sum over due engine views (`design_engine_core.md` §3.1), so the number of terms must be known at construction. 64 is chosen to be far from binding: render, GUI and a few analytics taps come nowhere near it. The cap closes the sum; it is not meant to restrain a design. Each engine view's row width is uncapped (§5); the cap is on how many terms there are. Raising it also raises the worst-case snapshot memory in proportion, because that is a sum over the same terms (§5) |
| Paced participants | **256** | See §2.1. Each participant can add up to its declared deadline to a tick. They wait concurrently, so the worst case is `max(deadline)`, not the sum, which is why the count can be generous. The cap makes the participant set enumerable, scanned once per tick; it is not a restraint. The host is not one of them, so the simplest session has none |

## 3. Storage: chunk quantum

The chunk quantum is the number of slots in one chunk. It is a build constant, set in one
place.

`LIBSIM_ESTAB__CHUNK_ELEMENTS = 1024`. It is a CMake cache variable, defined in one place
(`design_data_container.md` §4).

1024 is the default that `design_data_container.md` §4 proposes. It meets three constraints
at once. It is a power of two. It is at least `64 / sizeof(T)` for every column type. And
one chunk of a 4-byte column is about one page. A power of two of at least 64 is a multiple
of `64 / sizeof(T)` for every `T` up to 8 bytes. So chunk boundaries always land on
column-padding boundaries under the padding rule of `design_data_container.md` §3, and no
chunk starts in the middle of a vector.

Retuning it is a rebuild. A chunk is the unit of parallel-for work items, SIMD iteration and
false-sharing isolation. It has no spatial meaning and constrains no GPU workgroup, because
uploads are whole-column.

Each chunk also keeps a count of its live slots (`design_data_container.md` §4), so a scan
skips a chunk with none. One chunk of a 4-byte column is about one page, so the chunk is a
natural unit for memory as well. The data pages of a fully empty chunk may be decommitted.
The generation column is never decommitted, because a slot's generation must survive while
the slot is empty (`design_data_container.md` §2.2).

## 4. Object caps

Every object type has a cap: the most entities of that type alive at one time. This section
gives the default and the maximum, and says what a cap bounds and what it does not. The
mechanism, including what a create at the cap does, is in `design_data_container.md` §2.1
and §2.2.

| Value | Constant | Reasoning |
|---|---|---|
| default cap | **2²⁴** (16,777,216), `default_entity_capacity` | The cap of every object type that declares none (below) |
| maximum cap | **2³²−1**, `max_entity_capacity` | A slot is a `u32`, and it is the high 32 bits of a `u64` id (`design_data_container.md` §2.2). A larger cap has no slot to put an entity in |

Both are build constants. A type sets its own cap with `[[=cap(N)]]`, and a session may
override it per type with `EngineConfig.entity_capacity`. Every cap is closed at the freeze
and joins session identity.

**Why 2²⁴.** The default cap is a ceiling: reaching it means a runaway system, not normal
play (`design_data_container.md` §2.2). So it must sit well above any real population and
well below the end of memory. At 64 bytes per row, 2²⁴ entities take 1 GiB, so a runaway
system stops long before the machine runs out. The budgeted scale is 10⁶ live rows, and 10⁷
is reachable with row predicates (§5). Both are under the default. A type that legitimately
needs more declares its own cap.

**A large cap costs address space, not memory.** At the freeze the engine reserves address
space for `cap × element size` for every column. It commits pages as the pool's extent
reaches them (`design_data_container.md` §2.2). A 2²⁴ cap of 8-byte values reserves 128 MiB
per column. A thousand such columns use about 125 GiB of the 128 TiB a 64-bit Linux process
can address. Addresses never move for the whole session.

**Commit failure ends the session; it is never a rejection.** A failed commit enters
`failed` (`design_python_api.md` §2). Refusing creates because one machine ran out of memory
would make the accepted set depend on the machine, and replays would diverge. On Linux,
overcommit can let a commit succeed and fail later, so the engine commits explicitly and
counts committed bytes against a budget.

**A cap bounds rows, not row width.** Neither the number of object types nor the columns per
type is capped, by decision. Capping types restrains a schema, and capping columns restrains
what a mod may model. Row width is watched where it costs: projection width in §5, and
dynamic columns by attribution below.

**Attribution is kept, because it is not about a bound.** Every column of a type is reserved
at the cap, zeroed at every erase and written at every create (`design_data_container.md`
§2.2). So a Tier 3 mod that registers dynamic columns raises the memory and per-entity cost
of a core object type (`design_data_container.md` §7.3). That document requires such a cost
to be bounded and attributable. Nothing caps it, so attribution carries the whole
requirement. **Registered dynamic columns are counted and reported per (object type,
mod).** A third party that inflates a core type's cost is named, even though nothing refuses
it. Where the counts are reported is open
([Q36](open_question.md#q36-where-are-per-type-mod-attribution-counts-reported)). An
expensive mod is diagnosed, not rejected, but it is never anonymous.

## 5. Projection: uncapped on purpose

**There is no maximum projected-row width, and this is a decision, not an omission.**
Projection cost is measured per engine view and checked against a warning threshold. The row
predicate, not a width cap, is what keeps large worlds affordable.

A per-row cap bounds the wrong axis. An object type may legitimately be complicated. Capping
row width would restrain the schema and leave the quantity that governs cost, `width ×
matched rows`, as large as before. At 10⁶ live rows a narrow 32-byte projection is already 32
MB per publish; at 10⁷ it is 320 MB. A smaller row cannot fix either number. Projecting
fewer *rows* can. That is why the row predicate (`design_engine_core.md` §3.4) carries this
axis and a width cap does not.

What is capped is the number of projections: 64 engine views (§2.3). Each engine view holds 3 blocks, one
exchange word and a fixed `ret[3]` return header (`design_engine_core.md` §3.5). Total
snapshot memory is `Σ over engine views (3 × spec × matched rows)`, plus that small fixed control
storage. The sum has a fixed number of terms. Each term is a declared column subset over a
declared row predicate, not one uncapped payload multiplied by a pool size. Each block
reserves address space for its engine view's maximum rows and commits pages only as rows match
(`design_data_container.md` §5.1).

So projection is bounded in rows and measured in cost. Its rows are bounded by the object
type's cap, and an engine view's matched rows by the engine view's maximum rows
(`design_data_container.md` §5.1). Its width is not capped. **The cost is measured, and a
crossing is diagnosed.**

- `publish duration` and `publish bytes/second` are first-class metrics, **per engine view and in
  total** (`design_engine_core.md` §3.1). The byte rate is the CPU-side counterpart of the
  `upload bytes per frame` metric that section also requires. Per-engine-view attribution makes a
  crossing actionable: it names the declaration to narrow.
- The warning threshold is **4 GB/s, checked per engine view**
  ([Q42](open_question.md#q42-the-warning-thresholds)). It is a bandwidth, not bytes per
  publish, so it does not depend on the tick rate or on an engine view's cadence. A cadence hint can
  change the cadence while the session runs.

  **Per engine view, because a session total cannot name a remedy.** Every remedy in this section
  applies to *one declaration*: narrow the spec, tighten the predicate, lower the cadence. A
  threshold on the sum says only that the session is expensive. It leaves the reader to find
  which of up to 64 engine views is responsible, and naming that engine view is the diagnosis the metric
  exists to deliver.

  **Why 4 GB/s.** One unfiltered engine view over 10⁶ live rows at a 32-byte spec is 0.96 GB/s at
  30 Hz, and 10⁶ rows is the scale this design budgets for. A threshold at 1 GB/s would sit
  inside that normal case. It would fire continuously in a session doing nothing wrong, and
  a warning that always fires is no better than none. 4 GB/s leaves the budgeted case quiet
  and still catches the same engine view at 10⁷ rows (9.6 GB/s). It also stays far below what a
  memcpy-bound machine can move, so a crossing means an engine view is pathological, not merely
  large. The threshold is separate from the GB/s trigger in `design_data_container.md` §5.
  That trigger is for revisiting whole-column upload, which is a GPU-bandwidth question, not
  this one.

  **This number is reasoned, not measured**, which makes it the weakest value in this
  document. It is reasoned from this design's own budgeted scale. §9 gives its revisit
  trigger.
- Crossing it is diagnosed: a warning-class event naming the measured rate and the engine view. It
  is never a rejection and never a silent degradation. The simulation does not change.
- **`matched rows` is a first-class counter per engine view.** It is the factor the row predicate
  exists to move, and usually the one a bandwidth crossing traces back to. An engine view whose
  predicate is `ALL` reports every live row, and says so.
- **Consumer lag** is `last published tick − last_consumed_tick`, read from the return
  header (`design_engine_core.md` §3.5). It is the counter for a slow reader. A `PRIVATE`
  engine view never skips a publish, so it has no skip counter like the `SHARED` mode's
  `publish_skipped_pinned` (`design_engine_core.md` Appendix A). Without lag, a lagging
  reader would be invisible: the publisher simply overwrites and nothing fails. Sustained
  lag is fixed by narrowing the projection, tightening the predicate, lowering the cadence,
  or by the reader sending a cadence hint of its own.

**Both projection axes are declared.** Over columns, each engine view declares a column subset, so
a renderer, an agent and a mod each pay for what they read (`design_engine_core.md` §3.1).
Over rows, each engine view declares a predicate *kind*, whose parameters can change on every
publish (`design_engine_core.md` §3.4). A renderer wants what is on screen; an agent wants
what is near it. The row axis is what makes 10⁷ live rows reachable at all, because at that
scale no reader wants every live row. It reuses existing machinery. Registration is the
declaration site, and the tick budget of `design_engine_core.md` §3.1 is the cost model. The
algorithm is the publish scan and gather of `design_data_container.md` §5.1: the live
bitmap ANDed with the predicate, an exclusive scan, and a gather, emitted by the same
generator.

**What remains is the scan, not the gather.** A predicate answered by a linear pass still
reads its input columns for every live row: 12 bytes per row for a position test, reads only
([Q61](open_question.md#q61-what-row-width-does-the-predicate-scan-assume)). The predicate
shrinks the gather term; the scan term is the floor. The two ways below it are a spatial
index behind the predicate kind, and running scan and gather on the worker pool. Neither is
decided here. Where the scan is charged is open (§7).

## 6. Checksum algorithm and levels

The checksum detects desyncs. This section sets when it runs, at three levels, and picks the
one algorithm all three use. It also says why that algorithm is written by hand.

**Three levels, from cheap and always on to full and test-only.** A full hash of the world
every tick is the strongest check and the most expensive. Only tests need it every tick.

| Level | What it hashes | When it runs | Cost |
|---|---|---|---|
| 1. **tick digest** | The commands drained this tick, in drain order, plus each object type's live count and extent | Every tick, always | O(object types + commands) |
| 2. **rolling checksum** | Each tick, the chunks whose index ≡ tick (mod N), for every object type: columns, generations and bitmaps. The whole world is covered every N ticks | In multiplayer, and while a replay is recorded | 1/N of a full pass per tick |
| 3. **full checksum** | The whole world every tick, plus one hash per system over the columns it declares as writes, taken when its phase closes | CI golden replays, certification, desync bisecting | A full pass per tick, plus the per-system hashes |

The tick digest catches a divergence in input or in the live set on the tick it happens. The
rolling checksum catches a divergence in values within N ticks. The full checksum names the
system that caused it. A single-player session that records nothing needs only the tick
digest. Derived columns (`design_data_container.md` §2.1) are left out of levels 1 and 2.
Level 3 rebuilds them and compares.

The rolling period N is proposed at 30, one second at 30 Hz, but it is not decided
([Q67](open_question.md#q67-the-rolling-checksum-period-and-the-tick-digest)). The tick
digest's exact contents are part of the same question.

**One algorithm: an order-independent reduction, with each row's id mixed in.**
`design_engine_core.md` §4.2 names two correct options, and this is the one chosen, for two
reasons:

- **It splits per chunk.** Each chunk's contribution is computed on its own, and the results
  combine in any order. So level 3 runs in parallel over chunks, and level 2 can hash one
  slice of chunks per tick. A hash streamed in row order can do neither.
- **It catches swapped values.** Two entities' values swapped is the typical failure of an
  iteration-order bug, and a plain order-independent reduction cannot see it
  (`design_engine_core.md` §4.2). Mixing the id into each row's contribution can. The id
  contains the slot, so the same mixing also covers where each entity sits.

The inputs are state: the columns of live rows, plus each pool's generation column, live and
retired bitmaps, and extent (`design_data_container.md` §2.2). A slot's generation decides
the next id issued there, so it affects the future and is hashed.

The function is a hand-written 64-bit mix, not an external hash. The reasons, in order:

- It is bit-exact across compilers and platforms, because every operation is our own. This
  matters most, and a dependency cannot guarantee it across its own versions.
- It is `constexpr`-testable.
- It has no Conan recipe to pin.
- It vectorizes over SoA columns. A byte-stream hash API would force per-row serialization,
  against the grain of the container.

The bit-level specification is part of M2, with `fixed<>`, and is open
([Q27](open_question.md#q27-the-checksums-exact-algorithm-and-inputs)).

## 7. What this document does not decide

**Ten things are open on purpose.** Each has an entry in
[open_question.md](open_question.md). Each site that would otherwise look unfinished says so
and says what the value waits on. A reader who finds a missing number should find the reason
next to it, without coming here to learn whether anyone noticed.

| Open | Decided by | Why not here | Noted at |
|---|---|---|---|
| **`input_delay_ticks`** ([Q50](open_question.md#q50-input_delay_ticks)): how far ahead of the current tick a peer stamps, that is, its endpoint's stamp margin | a transport | Unlike every number above, it needs measurement against a real round-trip time. It is not an engine value: the engine imposes no margin and bounds none. No ring depth depends on it | `design_multiplayer.md` §3.2, §7; here §1.1 (`ipc_deadline`) |
| **`ipc_deadline`** ([Q43](open_question.md#q43-ipc_deadline-value-and-constraints)) | the same measurement | It is not a free parameter. Its ceiling is the input delay, so it lands with the row above and not before | §1.1 |
| **The mod gate deadline**, default and cap ([Q80](open_question.md#q80-the-mod-gate-deadline-default-and-cap)) | M7 | It trades tick latency against how often a busy mod is suspended, and needs a measured mod-loop time | §1.1 |
| **Per-slot size**, and so total slot memory ([Q22](open_question.md#q22-how-large-is-one-command-or-event-slot)) | M1 | No doc gives a per-slot size for a command or an event entry, on purpose, so the worst case in §2.2 is a count, not a number of bytes | §2.2 |
| **An engine view's maximum rows, and the GPU allocator behind it** ([Q39](open_question.md#q39-the-gpu-allocator-for-growing-worlds)) | before M3 | Destination buffers cannot be resized after creation, so each is sized from its engine view's maximum rows, not from the current row count. The default maximum, how an engine view declares it, and whether destinations suballocate from a few large buffers need measurements. Choosing now would be guessing | `design_data_container.md` §5, §5.1 |
| **The rolling checksum period N**, and the tick digest's exact contents ([Q67](open_question.md#q67-the-rolling-checksum-period-and-the-tick-digest)) | M2 | 30 ticks, one second, is proposed. N trades detection delay against per-tick cost, and a number with no profile behind it would read as an answer | §6 |
| **`commands_per_tick`** as a manifest field ([Q44](open_question.md#q44-what-commands_per_tick-in-a-manifest-means)) | a design step before M2 | The example manifest requests 4096, and this document caps endpoint capacity at 256. Both stand. The field's *meaning* is not settled, and reconciling the numbers first would settle the wrong question | `design_modding.md` §3; here §2 |
| **Where the predicate scan is charged** ([Q35](open_question.md#q35-where-is-the-predicate-scan-charged)) | a design step before M3 | The row axis itself is decided (`design_engine_core.md` §3.4). What is not decided is whether the scan is sim-thread work in the tick budget of `design_engine_core.md` §3.1, or runs on the worker pool. At 10⁷ live rows it is roughly a third of a tick ([Q61](open_question.md#q61-what-row-width-does-the-predicate-scan-assume)). Moving it makes publish the first step after the terminal commit to run on the worker pool, which touches the phase structure of `design_engine_core.md` §4.1 | §5 |
| **The PRNG generator** ([Q26](open_question.md#q26-the-prng)) | M2 | M1 needs only the seed in the header. The generator itself is what M2 specifies | §6 |
| **Peer participant deadlines, and the escalation behind `on_expiry = DROP`** ([Q49](open_question.md#q49-peer-deadline-and-drop-escalation)) | a transport and session protocol | `design_engine_core.md` §3.3 fixes the shape. A local timeout may never remove a peer on its own authority, and a drop must take effect at an *agreed tick* carried in the stream. The value and the escalation belong to the multiplayer design | `design_multiplayer.md` §4.1, §7 |

One more item is not a number but is due in the same period, before M3: **Windows and
C++26** ([Q34](open_question.md#q34-windows-and-c26)). Conan restricts `msvc.cppstd` to
`[null, 14, 17, 20, 23]`, so C++26 cannot be expressed on the Windows profiles.

**Unloading a suspended mod** is open too
([Q47](open_question.md#q47-unloading-a-suspended-mod)). It is noted where it belongs,
`design_modding.md` §4.2, and not here: it is a lifecycle question, not a value.

## 8. Landing sites

This section names the one place each value lives. The C++ side is the partition
`sim_estab:limits`, in `src/libsim_estab/module/sim_estab--limits.cppm`, re-exported by
`sim_estab.cppm`. The Python side is `src/sim_estab/config.py`.

| Value | Python (`config.py`) | C++ (`sim_estab:limits`) | Notes |
|---|---|---|---|
| tick rate | `EngineConfig.tick_rate` | `default_tick_rate` | policy default |
| `D` | `CommandPolicy.drain_interval_ticks` | `default_drain_interval_ticks` | policy default |
| endpoint capacity, default | `CommandPolicy.source_capacity` | `default_source_capacity` | policy default |
| host endpoint capacity | `CommandPolicy.host_source_capacity` | `default_host_source_capacity` | policy default |
| peer endpoint capacity | `CommandPolicy.peer_source_capacity` | `default_peer_source_capacity` | policy default |
| endpoint capacity, engine cap | `MAX_SOURCE_CAPACITY`, a module constant that `CommandPolicy` checks against | `max_source_capacity` | build constant |
| player cap | — | `max_players` | enforced by session formation |
| source cap | — | `max_sources` | enforced at the freeze |
| participant cap | — | `max_paced_participants` | enforced at the freeze |
| Engine view cap | — | `max_views` | enforced at the freeze |
| host policy: shutdown deadline, retry limit, inbox cap | `HostPolicy.shutdown_deadline`, `HostPolicy.mod_retry_limit`, `HostPolicy.max_inbox_size` | — | policy defaults, Python only |
| chunk quantum | — | `chunk_elements` | build constant. Its one definition is the CMake cache variable `LIBSIM_ESTAB__CHUNK_ELEMENTS` in `src/libsim_estab/CMakeLists.txt`. The partition falls back to 1024 only for a build outside this CMake |
| projection warning bandwidth | `EngineConfig.projection_warn_bytes_per_second` | `default_projection_warn_bytes_per_second` | policy default. Checked per engine view, not against the session total (§5) |
| default entity capacity | `DEFAULT_ENTITY_CAPACITY` | `default_entity_capacity` | build constant, 2²⁴ (§4) |
| maximum entity capacity | `MAX_ENTITY_CAPACITY`, a module constant that `EngineConfig` checks against | `max_entity_capacity` | build constant, 2³²−1 (§4). The `u32` type of the C++ constant is what keeps a cap within a slot |
| per-type caps | `EngineConfig.entity_capacity` | — | per-type override, closed at the freeze (§4), each entry 1 to 2³²−1 |
| `C`, ring depth, `E`, `high_water`, event ring size | **nowhere** | **nowhere** | computed at the freeze from the closed endpoint, participant, engine view and object type sets (`design_engine_core.md` §2.4 step 3a). Writing any of them down would create the engine-wide pool that per-endpoint rings avoid |
| `ipc_deadline` | `HostPolicy.ipc_deadline`, default `None` (unbounded) | — | value open (§7). `EngineConfig` rejects a set value that does not exceed one tick at the configured rate |
| `input_delay_ticks`, per-slot size | **not yet anywhere** | **not yet anywhere** | values open (§7) |
| mod gate deadline, default and cap | `HostPolicy.mod_deadline_ms`, `HostPolicy.max_mod_deadline_ms`, **not yet in `config.py`** | — | values open (§7) |

Endpoint capacity and `D` are configurable at runtime, so only their defaults can be checked
with `static_assert`. `config.py` validates the configured values at construction, and the
freeze validates them again. The assertions in `sim_estab:limits` protect the defaults and
the relationships between constants, not the session. They check that:

- each default capacity is within the cap;
- `D` and the tick rate are positive;
- sources exceed players;
- participants do not exceed sources, which holds because both caps are 256 (§2.1);
- the engine view and participant caps are nonzero;
- the chunk quantum is a power of two of at least 64, and a multiple of one cache line of
  1-byte elements;
- the default entity capacity is positive and within the maximum.

`test/test_config.py` asserts the Python defaults as literals. A change to a default fails
that test, so the test, this document and `sim_estab:limits` change together.

## 9. Revisit triggers

Each value has a named trigger, so that revisiting it is a decision, not a drift.

| Value | Revisit when |
|---|---|
| `tick_rate` | Never casually. It is in the replay header, so a change invalidates every existing replay |
| endpoint capacity | `queue_full` reaches a producer that is not exceeding its own declaration. Capacity is per tick and per endpoint, so this means the declaration is wrong, not the engine |
| source cap | A legitimate session wants a 257th source. Raising it costs almost nothing (§2.1), so hitting it is information, not a wall |
| participant cap | The same, with more suspicion: every participant is one more way for the core to stop. Raising it does not admit more *pacing*. Submitting already paces a peripheral, so the cap bounds how many distinct things are paced, not whether they are |
| player cap | A game design wants more than 128 humans. It is a game rule, so this is a design decision, not an engine one |
| Engine view cap | A session legitimately wants a sixty-fifth engine view. The cap is meant not to bind, so hitting it is information |
| the `SHARED` engine view mode itself | A process-host mod needs a snapshot. The mode is deferred, not deleted (`design_engine_core.md` Appendix A). A revival should re-derive the copy-per-reader cost against the row counts of §5 before restoring the pin count as written ([Q60](open_question.md#q60-reviving-the-shared-engine-view)) |
| `D` | The event backlog reaches `high_water` under a host that is *not* defective |
| commands executed per tick | The metric (`design_engine_core.md` §3.1) approaches `C` in a session anyone intends to ship. A tick runs everything it is given, so this is the number that turns a busy session into a long tick |
| `HostPolicy` values | A legitimate mod host is being timed out |
| chunk elements | A profile shows chunk-boundary overhead, or a column type wider than 8 bytes is approved |
| default object cap | Most object types in a real schema declare a cap above 2²⁴. One type that needs more declares its own cap; the default moves only when it stops fitting the normal type |
| projection bandwidth | An engine view crosses `projection_warn_bytes_per_second` (§5: 4 GB/s, per engine view). Revisit also when a real profile exists: the figure is reasoned from 10⁶ rows × 32 bytes × 30 Hz, and nothing has measured it ([Q42](open_question.md#q42-the-warning-thresholds)). A threshold at 1 GB/s, checked against the session total, would sit below that budgeted normal case and fire continuously. The figure is independent of the upload trigger in `design_data_container.md` §5, which is separate and answers a GPU-bandwidth question |

---

## References

- `design_engine_core.md` §2.3 (identity), §2.4 (the freeze), §3.1 (engine views, the tick budget),
  §3.3 (participants and the gate), §3.4 (row predicate), §3.5 (return header), §4.1
  (phases), §4.2 (checksum), §5.1 (the command ring), §5.2 (the event backlog and the
  overflow policy), Appendix A (the `SHARED` engine view)
- `design_multiplayer.md` §3.2 (the input delay), §4.1 (the two network tiers), §7 (open
  questions)
- `design_python_api.md` §3 (`EngineConfig`), §4.1 (the main loop), §7.1 (endpoint capacity,
  `D`), §7.2 (snapshots), §7.3 (events)
- `design_data_container.md` §2.1 (annotations, caps), §2.2 (the pool, caps), §3
  (padding), §4 (chunk, live counts), §5 (upload cost and its GB/s trigger), §5.1 (the
  snapshot generator, the publish kernel), §7.3 (dynamic columns)
- `design_modding.md` §3 (manifest), §4.2 (inbox, suspension, quarantine), §4.3 (process
  hosts)
- Code: `src/libsim_estab/module/sim_estab--limits.cppm`, `src/sim_estab/config.py`,
  `test/test_config.py`
