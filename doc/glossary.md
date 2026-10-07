# Glossary

One name per concept, used the same way in every design doc. If a doc needs a term that is
not here, add it here first.

Doc names are shortened: `engine_core` is `design_engine_core.md`, `python_api` is
`design_python_api.md`, `data_container` is `design_data_container.md`, `modding` is
`design_modding.md`, `limits` is `design_limits.md`, `multiplayer` is
`design_multiplayer.md`, `patterns` is `design_patterns.md`.

## Time and session

| Term | Meaning | Where |
|---|---|---|
| tick | One fixed engine step: gate, command drain, phases, terminal commit, publish. The tick counter is the only simulation clock. | engine_core §3 |
| tick rate | Ticks per second, 30 by default. Fixed for a session and recorded in the replay header. Game speed is the time scale, not the tick rate. | limits §1 |
| tick budget | Wall-clock time of one tick (33.3 ms at 30 Hz) and the list of costs charged against it. Time waiting at the gate is not charged. | engine_core §3.1 |
| `first_unexecuted` | The next tick that has not run yet. Only the sim writes it. A command stamped for an earlier tick is too late. | engine_core §3.3 |
| time scale | Game speed: how fast ticks are run in wall-clock terms. A control call; never recorded. | python_api §4.3 |
| control call | An API call that changes how the engine runs but never touches world state: pause, resume, `step(n)`, `set_time_scale`, log level. Never a command, never recorded. | python_api §3 |
| session | The world from the freeze until `close()`. One per `Engine`. | engine_core §2.4 |
| engine state | `created`, `configuring`, `running` (optionally with the sim thread), `stopped`, `closing`, `closed`, plus the terminal states `load_failed` and `failed`. | python_api §2 |
| `stopped` | The engine state after `stop_sim_async()`: no tick runs again, but reads and `close()` still work. | python_api §2 |
| freeze | The single call (`start_session()`) that moves the engine from `configuring` to `running`. It closes every set that must not change during a session, computes session identity, and publishes tick 0. | engine_core §2.4 |
| session identity | What makes two sessions "the same": engine build, schema, content, mod set, every object type's cap, source ids and seed. Computed at the freeze and written to the replay header. | engine_core §2.3 |
| schema identity | The schema part of session identity: the descriptor hash, mod-added columns and content hashes. | data_container §7.4 |
| replay artifact | Everything needed to reproduce a session: the replay header plus every command recorded as it was consumed. A lockstep turn log is a replay artifact. | engine_core §2.3 |
| replay header | The session-identity part of a replay artifact. Checked at the freeze; any mismatch raises `ReplayIdentityError`. | engine_core §2.3 |
| golden replay | A replay artifact whose final checksum CI checks, used as a regression test. | engine_core §2.3 |
| playback | Running a session from a replay artifact. All endpoints are closed and the recorded commands are fed in instead. | python_api §4.2 |
| checksum | A hash of world state used to detect desyncs. One algorithm, used at three levels: the tick digest, the rolling checksum and the full checksum. | engine_core §2.3, §4.2, limits §6 |
| tick digest | The level-1 checksum, taken every tick: the commands drained this tick, plus each object type's live count and extent. | engine_core §2.3 |
| rolling checksum | The level-2 checksum, taken in multiplayer and while recording: each tick hashes one slice of the chunks, so the whole world is covered every N ticks. | engine_core §2.3 |
| full checksum | The level-3 checksum: the whole world every tick, plus one hash per system. Used by CI golden replays, certification and desync bisecting. | engine_core §2.3 |
| determinism | The same session identity and the same command stream always produce the same checksums. Re-running live producers may produce a different command stream; that is not promised to repeat. | engine_core §2 |
| M0 … M7 | Build milestones. M0: decided limits in code (done). M1: session, engine views, command ring, gate. M2: fixed-point, PRNG, checksum, replay. M3: world container. M4: phase scheduler. M5: engine view projection, row predicate, GPU upload. M6: first system, renderer, sim thread. M7: mod tiers. | `TODO.md` |

## Threads and waiting

| Term | Meaning | Where |
|---|---|---|
| core | The headless program: everything that runs without a window. It holds the engine core, in its own partition, and some of the peripherals, such as GPU compute. Not every peripheral is in the core: one may have its own module. Viz has one (`sim_estab.viz`), and the AI and modding parts may get one. The namespace `sim_estab::core` is named for the core. | patterns §1 |
| engine core | The deterministic simulation: fixed-point, integer-only, the only holder of world state. Also called "the sim". | engine_core §1 |
| peripheral | Anything outside the engine core: rendering, viz, AI, GPU compute, logic mods, network peers. Reads engine views and changes the world only by submitting commands. | engine_core §1 |
| determinism boundary | The line between the engine core (plus content and engine core mods) and the peripherals. | engine_core §1 |
| host | The application that owns the engine, usually the Python program. It is source 0. In single-player it is not a participant: its commands name no tick. "Mod host" is a different thing (see Mods). | engine_core §3.3 |
| owner thread | The thread that created the `Engine`. Only it may make lifecycle calls and pump the engine. | python_api §2 |
| owner-thread rule | The frame loop never waits on the engine; it polls. Only `step(n)` and the shutdown calls wait, and only on the sim or a deadline. | engine_core §3.3 |
| host endpoint | The host's own command endpoint: source 0. Its producer is the owner thread. Unpaced: its commands name no tick, and the first drain after a submit runs them. | python_api §7.1 |
| unpaced | Of an endpoint: its commands name no tick, so the gate never waits for it. The host endpoint and every logic mod's, in single-player; in a networked session their submits go through the turn assembler. | engine_core §5.1 |
| sim thread | The C++ thread that runs ticks once `run_sim_async()` starts it: always in a windowed session, and in a headless one that wants real time. | python_api §4.3 |
| sim-thread mode | Ticks run on the sim thread, started by `run_sim_async()`, while the owner thread submits, drains and, when windowed, renders. Every windowed session runs this way; a headless one does when it wants real time, and otherwise runs ticks in `step(n)`. | python_api §4.3 |
| participant | A peripheral the engine core waits for before each tick, up to that participant's deadline. Every producer that names ticks is one; others can opt in with `paced=True`. | engine_core §3.3 |
| recorder | A peripheral that submits nothing but must not miss a tick; it registers as a participant explicitly. | engine_core §3.3 |
| conjunction | The set of active participants the gate waits for: a tick runs only when all of them are ready. | engine_core §3.3 |
| observer | A peripheral the engine core never waits for. It only reads its engine view. A logic mod is one unless it paces its engine view. | engine_core §3.3 |
| gate | The one place the engine core waits: before each tick, until nothing blocks the tick (stop, event backlog, pause, or a participant that is not ready). | engine_core §3.3 |
| run grant | The host's `run_until` value: the engine core may run ticks below it. `U64_MAX` means running; pause and `step(n)` lower it. | engine_core §3.3 |
| `stop_requested` | Sticky flag that stops the engine core at the gate. Highest priority. | engine_core §3.3 |
| declare ready | A participant stores `ready_through = t` to say it has finished submitting for tick `t`. | engine_core §3.3 |
| deadline | How long the gate waits for one participant. Each participant declares one. A pause stops the clock. | engine_core §3.3 |
| expiry policy | What happens when a participant's deadline passes: `Fail`, `ContinueWithout`, `Suspend` or `Drop`. | engine_core §3.3 |
| gate mutex | `gate.m`: the mutex every gate input changes under. The sim re-checks under it before it parks, and tick-progress waiters park on it too. Held only for loads, stores and a notify. | engine_core §3.3 |
| wake-up rule | Every predicate a parked thread waits on changes only while holding that park's mutex, followed by a notify. Every park in the engine follows it, so no wake-up can be lost. | engine_core §3.3 |
| mechanism register | The table of every cross-thread mechanism in the engine, each with its atomics, memory orders, linearization point, progress guarantee and failure behaviour. | engine_core §1.1 |

## Engine views and snapshots

| Term | Meaning | Where |
|---|---|---|
| engine view | The only way data leaves the engine core. A channel from the sim to one reader, made of three blocks and one atomic word. Registered before the freeze. `EngineView` in C++; in Python, the object `engine.view()` returns. | engine_core §3.1, §3.2 |
| `PRIVATE` mode | The only engine view mode in v1: one trusted reader, three blocks, no copying. | engine_core §3.1 |
| `SHARED` mode | A deferred engine view mode for many readers, where each reader copies. Designed but not built. | engine_core Appendix A |
| on-demand snapshot | What `snapshot()` returns: every `[[=viz]]` column of every live row, with `id`, copied in one pass into the snapshot buffer. The owner thread makes it, while no tick can run. No engine view stands behind it. | engine_core §3.1 |
| snapshot buffer | The one engine-owned buffer that every `snapshot()` call refills. Reserved at the freeze. A call is refused while array views of the previous result are alive. | engine_core §3.1 |
| executor | The thread that runs ticks: the sim thread, or the thread inside `step(n)` when there is no sim thread. Only it touches engine core state while a tick can run. | engine_core §3.3 |
| `identity=False` | An engine view's declaration that it does not carry the `id` column. Closed at the freeze. | engine_core §3.4 |
| block | One of an engine view's three payload buffers. The publisher, the reader and the "latest" slot each hold one at a time. | engine_core §3.2 |
| publish | After a tick, the sim fills its writable block for each due engine view and swaps it in. Never waits, never skipped. | engine_core §3.2 |
| take | The reader swaps its old block for the newest published one. | engine_core §3.2 |
| snapshot | Read-only, float-converted data for one tick, held in a block or copied out. Not a savegame. | engine_core §5 |
| projection | What publish copies for one engine view: which columns (the projection spec), which rows (the row predicate), converted from fixed-point to float. | engine_core §3.1 |
| projection spec | The column list an engine view declares. | engine_core §3.1 |
| row predicate | An engine view's row filter. Its kind (`ALL`, `AABB`, `SPHERE`, `FRUSTUM`, `TAG`) is fixed at the freeze; its parameters can change on every publish. Coarse in the engine core, refined by the reader. | engine_core §3.4 |
| return header | A small struct the reader writes before a take and the publisher reads afterwards: predicate parameters, last tick consumed, cadence hint. It can shape the engine view, never the world. | engine_core §3.5 |
| cadence | An engine view is published every `k` ticks. | engine_core §3.1 |
| cadence hint | A reader's request to be published less often. The publisher may ignore it, and always does on a paced engine view. | engine_core §3.5 |
| paced engine view | An engine view registered with `paced=True`. Its reader is a participant: each take declares it ready through the next publish. | engine_core §3.1 |
| consumer lag | Last published tick minus the reader's last consumed tick. The signal that a reader is falling behind. | engine_core §3.5 |
| array view | A NumPy view into a taken block. Not an engine view. | python_api §7.2 |

## Commands and events

| Term | Meaning | Where |
|---|---|---|
| command | The only input that changes world state. Submitted through an endpoint, run at one tick and recorded. A paced command names its tick; a host command takes the tick of the drain that runs it. | engine_core §5.1 |
| source | Anything that submits commands: the host, a mod, a peer, an engine AI. | limits §2.1 |
| source id | A source's `u32` number, fixed at the freeze. The host is 0. Commands run in source id order. | engine_core §2.4 |
| endpoint | A single-producer command ring for one source. A source has one endpoint in v1. | engine_core §5.1 |
| producer | The one thread that submits on an endpoint. | engine_core §5.1 |
| command ring | An endpoint's ring buffer. Its depth is `capacity` at margin 0 and on the host endpoint, `2 × capacity` at a margin of 1 or more. Computed at the freeze. | engine_core §5.1 |
| capacity | The most commands one endpoint may hold for one tick. Default 64, maximum 256. Exceeding it returns `queue_full`. The only number an endpoint declares. | limits §2 |
| stamp | The tick a command names. | engine_core §5.1 |
| stamp margin | How far past the current tick an endpoint may stamp, and so how far ahead its producer may declare ready. 0 for engine sources; the input delay for a peer. The unpaced host endpoint stamps nothing. | engine_core §5.1 |
| admission | The immediate answer to a submit: `admitted` with a handle, or a rejection (`queue_full`, `too_late`, `out_of_order`, `over_margin`, `invalid`, `revoked`, `host_error`). | engine_core §5.1 |
| `too_late` | The named tick is past, or its producer has already declared it ready. | engine_core §5.1 |
| admission wait | A paced submit that finds its ring full waits, on the endpoint's mutex and condition variable, until the oldest tick in the ring has run. Only a ring with a margin can fill. A pause leaves it parked; only revocation wakes it early. | engine_core §5.1 |
| endpoint lease | An endpoint's lifecycle state, which every submit enters and which `revoke` closes. | engine_core §5.1 |
| outcome | The second answer to a command: whether it was applied when its tick ran. Read from the handle after that tick. For a host command it also reports the tick, and reads `pending` until then. | engine_core §5.1 |
| deadlock rule | A paced producer may wait only for outcomes of ticks it has already declared ready. Breaking it raises `CommandOrderError`. | engine_core §5.1 |
| command drain | At the start of tick `t`, the sim takes every command stamped for `t` from every endpoint, in ascending source id. | engine_core §5.1 |
| sequence | A command's position in the drain. With the source id it gives every command a total order, with no sort. | engine_core §5.1 |
| C | Total commands the engine may run per tick: the sum of all endpoint capacities. Computed at the freeze, never configured. | limits §2 |
| event | Something the engine reports about itself, stamped with its tick: the session, the gate, participants, endpoints, engine views and caps. A change in the world is never an event; readers see it as state. The M1 list is in python_api §7.3. | engine_core §5.2 |
| event ring | The single-producer queue carrying events from the executor to the session's event drain. It holds `E × (D + 1)` entries and cannot overflow. | engine_core §5.2 |
| event drain | Who empties the event ring. Every session declares one in `EngineConfig.event_drain`; `start_session()` refuses a session without one. | python_api §3 |
| owner drain | An event drain the owner thread empties with `drain_events()`, between ticks. Every windowed session has one. | engine_core §5.2 |
| sink drain | An event drain the engine empties itself after every tick and when a step ends, into a native sink that counts and hashes the events and keeps nothing. For CI and golden replays. | engine_core §5.2 |
| E | The most events the engine creates itself in one tick: `T·S + S + 3·P + V + 4`, from object types, sources, participants and engine views. Computed at the freeze. | engine_core §5.2 |
| D | The drain interval: how many ticks of events the event ring is sized to hold between two `drain_events()` calls. Default 8. | limits §2 |
| event backlog | Events the drain has not consumed yet. A plain count owned by the executor. | engine_core §5.2 |
| side list | Where events raised outside a tick wait, under their own mutex, until the drain merges them in. Each carries the next tick to run, and comes before that tick's ring events. | engine_core §5.2 |
| `high_water` | The event backlog size at which the gate pauses the sim: `E × D`, derived at the freeze. Under an owner drain a `step(n)` that reaches it raises `EventBacklogError` instead. | engine_core §5.2 |
| delivery class | How an event may be lost: reliable (never), coalescible (only the latest kept), best-effort (may drop). | engine_core §5 |

## World data

| Term | Meaning | Where |
|---|---|---|
| object type | A kind of entity (for example `pop`), stored as one pool. | data_container §2.1 |
| pool | How every object type is stored: columns indexed by slot, with holes where entities were erased. Rows never move. | data_container §2.2 |
| column | One array holding one property for every row of an object type. | data_container §3 |
| column id | A column's `u32` number, returned by `resolve_column` and fixed at the freeze. Not an entity id. | data_container §7.3 |
| column span | The slice of a column a system may access, sized by its access kind. | data_container §4 |
| slot | A position in a pool. An entity keeps its slot for its whole life. After an erase the slot may hold a new entity. | data_container §2.2 |
| row | An entity's slot, used as an index into its columns. A bare row cannot tell a new occupant from an old one, so it never leaves the engine core. | data_container §2.2 |
| generation | A slot's `u32` reuse counter. It starts at 1 and goes up by one at each erase. | data_container §2.2 |
| id | An entity's `u64` number: its slot in the high 32 bits and its generation in the low 32. No id value is issued twice. The only way commands, snapshots and saves refer to an entity. | data_container §2.2 |
| live bitmap | One bit per slot of a pool, set while the slot holds an entity. | data_container §2.2 |
| extent | One past the highest slot a pool has used. It never shrinks during a session. Scans cover `[0, extent)`. | data_container §2.2 |
| retired slot | A slot whose generation reached its maximum. It is never used again, so no id is issued twice. | data_container §2.2 |
| append-only object type | An object type declared `[[=append_only]]`: it has no erase, so every slot below its extent is live. | data_container §2.1 |
| derived column | A column declared `[[=derived]]`: a cache rebuilt from other state. Not saved, and not in the tick digest or the rolling checksum. | data_container §2.1 |
| cap | The limit on the live entities of one object type, fixed at the freeze: `[[=cap(N)]]` or `EngineConfig.entity_capacity`. Default 2²⁴, at most 2³²−1. | data_container §2.2 |
| game-rule cap | A cap the game relies on: a create at the cap is rejected deterministically. | data_container §2.2 |
| ceiling | A cap used as a safety limit, the default kind: hitting it stops and reports. | data_container §2.2 |
| chunk | A fixed run of 1024 slots: the unit of SIMD and parallel work. Each chunk keeps a live count, so empty chunks are skipped. | data_container §4 |
| body | An agent's world-facing state (position, resources, health). Engine core state, updated deterministically. | engine_core §6 |
| mind | Where an agent decides. Inside the determinism boundary it is engine core logic; outside it is a peripheral that submits commands. | engine_core §6 |
| system | A deterministic update function. Declares how it accesses each column. | engine_core §4.1 |
| access kind | `read_full`, `read_local`, `write_local`, `write_staged` or `write_dbl`. | engine_core §4.1 |
| phase | A run of systems within one tick that can execute in parallel safely. Phases are computed at the freeze from the access kinds. | engine_core §4.1 |
| phase cut | The rule that splits the system list into phases. | engine_core §4.1 |
| boundary commit | At the end of each phase: merge staged writes and flip double-buffered columns. Never changes row counts. | engine_core §4.1 |
| terminal commit | Once per tick, after the last phase: every erase, then every create. The only place the live set changes, so an entity created this tick has no row until the commit. | engine_core §4.1 |
| relationship | A link between object types, stored as slots inside the engine core. Not designed yet. | data_container §2.2 |
| dynamic column | A column an engine core mod adds before the freeze. | data_container §7.3 |
| `fixed<>` | The project's fixed-point number type, the only arithmetic allowed in world state. | engine_core §2.1 |

## Mods

| Term | Meaning | Where |
|---|---|---|
| Tier 1 mod | Content: data only. | modding §1 |
| Tier 2 mod | Logic: Python outside the engine core, acting only through commands. Also called a logic mod. Its commands name no tick, so it never holds the session; it is a participant only through a paced engine view. | modding §1, §3 |
| Tier 3 mod | Engine core: native code inside the tick, trusted. Also called an engine core mod. | modding §1 |
| mod host | Where a Tier 2 mod runs: a thread host (in-process, the default) or a process host (a separate process). | modding §4 |
| supervisor | The engine-side thread that runs each process host: its endpoint's single producer. | modding §4.3 |
| containment unit | The OS object that limits a process host: a Job object on Windows, a cgroup on Linux. | modding §4.3 |
| manifest | A mod's `mod.toml`: id, version, tier, host kind, dependencies, capabilities, limits. | modding §3 |
| principal | The named party behind a source: a mod's manifest `id`. | modding §3 |
| capability | What a manifest says a mod may do: command types, event topics, columns to read. Policy can only remove capabilities. | modding §3 |
| `ModPolicy` | The user's choice of which mods load and what they may do. An argument to `load_mods`, never recorded. | modding §3.1 |
| `HostPolicy` | Limits the engine enforces on mod hosts, part of `EngineConfig`. | python_api §3 |
| `ModContext` | The only API a Tier 2 mod uses. | modding §4.1 |
| mod bus | Delivers engine events to one inbox per mod. | modding §4.2 |
| inbox | A mod's event queue. Its size is capped by `max_inbox_size`. | modding §4.2 |
| suspend | A mod whose inbox overflows stops being fed and stops being waited for, but keeps its endpoint. Reversible. | modding §4.2 |
| quarantine | A mod that fails is stopped, and leaves the gate at once. `mod_retry_limit` sets how often it is restarted. | modding §4.2 |
| host services table | The versioned C ABI the engine gives a Tier 3 mod. | modding §5.1 |
| certification | Checking that a Tier 3 mod is deterministic by re-running golden replays at 1 and N threads, in Debug and Release. | modding §5.3 |

## Multiplayer

| Term | Meaning | Where |
|---|---|---|
| lockstep | Only commands cross the network; every peer simulates everything. | multiplayer §1 |
| peer | A remote player: a participant with its own source id, endpoint and receive thread. | multiplayer §3.1 |
| turn | The set of commands for one tick, sent as one network message. One turn is one tick. | multiplayer §5 |
| input delay | A peer endpoint's stamp margin (`input_delay_ticks`). Value open. | multiplayer §3.2 |
| server tier | The few server nodes that alone may decide a peer has dropped, and at which turn. | multiplayer §4.1 |
| authority | The one node that computes and broadcasts commands from a non-deterministic peripheral. For a logic mod, the mod client that runs it. | multiplayer §4 |
| turn assembler | The part of the transport that builds a machine's outgoing turns. It stamps a tick-less submit (the local player's, a logic mod's) with the next open turn plus the source's margin, and never waits for the source. | multiplayer §3.2 |
| mod client | A headless engine instance, run by the operator, that joins a networked session as a client node only to run logic mods. It sits next to a server node in the session's most central region. | multiplayer §4.2 |

## Errors and shutdown

| Term | Meaning | Where |
|---|---|---|
| `sim_estab_error` | The root of the library's exception family. Every error the library throws derives from it, through its subsystem's own error (`log_error`, `gpu_error`, `viz_error`). Bound to Python under the same name. | patterns §6, §8 |
| shutdown sequence | `mods.stop()`, then `stop_sim_async()`, then `close()`. `close()` does any step not done, never raises because shutdown failed, and can be called twice. | python_api §2 |
| shutdown step | One of the three calls of the shutdown sequence. | python_api §2 |
| shutdown stage | One of the five blocking waits during shutdown, each bounded by `shutdown_deadline`: running submits on a revoked endpoint, a mod host's inbox drain and `on_unload`, mod host and sim thread joins, confirming a killed process is dead, and running engine calls in `close()`. | limits §1.1 |
| operation lease | Every native call except `close()` registers itself; `close()` refuses new calls and waits for running ones. | python_api §2, §6 |
| control block | The small object behind the operation lease. It outlives every native resource it guards, so a stale handle can still see that the engine is closed. | python_api §2 |
| registered dependant | Anything that can outlive a tick and still touch engine memory (mod hosts, engine view blocks held by array views). `close()` unwinds them before releasing anything native. | python_api §2 |
| staged join | How `stop_sim_async()` stops the sim thread: request stop, wake the gate, wait, then log and retry once before `failed`. | python_api §4.3 |
| `shutdown_deadline` | How long each blocking shutdown step may wait. 5 seconds. | limits §1.1 |
| `failed` | Terminal state when something could not be stopped safely. Resources are leaked on purpose and no new `Engine` may be created in the process. `on_failed_stop` decides whether it raises `EngineFailedError` or aborts. | python_api §2 |
| resource failure | A lost GPU device or a failed memory commit in the terminal commit. Not a `failed` path: the engine cleans up its child processes and the process exits with a nonzero status. | python_api §2 |
| `load_failed` | Terminal state after a failed load or freeze. Nothing leaks; create a new `Engine`. | python_api §2 |
| disarm | Cleanup that deliberately releases nothing, because another thread may still use the resource. | patterns §4 |
| detach | Disarm for one resource: an engine view block still referenced at `close()` is freed when its last array view drops. | python_api §7.2 |
| async error | An error on the sim thread, stored once and re-raised on the owner thread at its next engine call. | python_api §8 |
| rendezvous point | An owner-thread engine call where a stored async error is raised: a pump call, `stop_sim_async()` or `raise_if_failed()`. | python_api §8 |
| quiescent | The sim is parked and no tick is running, so state can be read safely (for example `checksum()` after `step()`). | python_api §4.3 |
| `ipc_deadline` | How long one process-host round trip may take before it becomes `host_error`. Value open. | limits §1.1 |
| catch-up clamp | The most real time one wake of the sim thread makes up after a stall: 0.25 s, applied before the time scale, so a requested speed is never clamped. A build constant. | limits §1.2 |
| build constant | A limit fixed when the library compiles, such as the chunk size. Changing it needs a rebuild. | limits |
| policy default | The shipped default of an `EngineConfig` field, which a session may override. | limits |
| warn threshold | A bandwidth above which the engine logs a warning, never a rejection: projection, per engine view. | limits §5 |

## Words with a preferred meaning

Some common words have more than one natural sense. This table gives each word a preferred
meaning, and a replacement for the other senses. It guides prose and binds identifiers.

- **Prose is free.** A word may keep any plain sense where the sentence makes that sense
  clear. "A session admits up to 128 players" and "a call admitted before `CLOSING`" are
  both fine, although "admission" is the answer to a submit. The replacement helps where
  the sense would be unclear. A row that says "never" is the exception: it holds in prose
  too.
- **Identifiers keep the preferred meaning.** A type, function, field or enum value uses a
  word of this table only in its preferred meaning, and takes the replacement for another
  sense. Code copies an identifier, and a search for it must find one mechanism.

| Word | Preferred meaning | For the other senses, prefer |
|---|---|---|
| host | the host application (source 0) | "mod host" (where a mod runs); "mod client" (a machine that runs logic mods in a networked session); "the engine" (for Tier 3 mods); "owner thread" (the thread) |
| owner | owner thread | "reader" (an engine view's single reader); "authority" (multiplayer) |
| phase | a compute phase within a tick | "sim-thread mode"; "shutdown step"; "engine state"; "registration" / "session start" (Tier 3 mods) |
| tier | mod tiers | "server tier" / "client nodes" (multiplayer) |
| admission | the answer to a submit | "endpoint lease"; "operation lease"; "id lookup" (resolving an id to a row) |
| gate | the tick gate | "endpoint lease"; "operation lease" |
| core | the headless program, and its namespace `sim_estab::core` | "engine core" (the deterministic simulation); "Boost.Log core" (the logging library's own object) |
| capacity | per-endpoint capacity, when unqualified. The prefixed API name `entity_capacity` (`EngineConfig.entity_capacity`, `default_entity_capacity`) sets an object type's cap | "cap" (object type, in prose); "event ring size"; "inbox size" |
| generation | a slot's reuse counter, the low half of an id | "epoch" (the `SHARED` engine view's published word) |
| `commands_per_tick` | only the manifest field | "C" (engine-wide total); "commands executed per tick" (the metric) |
| shared | the `SHARED` engine view mode, and the OS term "shared memory" | "one copy" (a result that several readers receive) |
| engine view | the engine mechanism. Never shortened to "view", in prose or in an identifier (`engine_view`, `EngineView`). Two exceptions already say "engine": the accessor `engine.view(name)`, through its receiver, and the partition `sim_estab:engine.view` with its namespace `engine::view`, through their `engine` prefix | "array view" (NumPy); "column span" |
| publish | engine view publish, and the release-store "publication" idiom of engine_core §1.1 | "fan out" (mod bus); "submit" (commands) |
| drain | command drain, `drain_events()`, inbox drain, per-worker log buffer drain | "wait for running calls" (leases at shutdown) |
| grant | run grant | "capability" (mods) |
| ledger | no preferred meaning; best avoided | "`first_unexecuted`"; "tick budget" |
| backlog | event backlog | "ticks owed" (catch-up) |
| event | an engine event, reported to the event drain | "a change in the world" (read as state); "outcome" (a command's answer); "input" (SDL input and window events from `poll_input()`) |
| M | milestones | "catch-up clamp" |
