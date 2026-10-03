# Multiplayer Design: Deterministic Lockstep

This document describes what networked play needs from the engine core. It covers only what
the command path must support. Transport, matchmaking, NAT traversal and rejoin are out of
scope (§7).

Status: not implemented. Lockstep adds no work to M1 (§6).

Terms are defined in [glossary.md](glossary.md). This design builds on
`design_engine_core.md` §1, §2, §3.3 (the gate), §5.1 (the command ring, its two waits and
the deadlock rule) and §6, and on `design_python_api.md` §3, §4.3 and §7.1.

---

## 1. The model

Multiplayer uses **deterministic lockstep**. Only commands are sent over the network. Every
peer simulates the whole world from the same command stream.

At our world size, lockstep is close to the only workable choice. A server-authoritative
design sends world *state*. At 10⁶–10⁷ live rows that is hundreds of megabytes per publish
(`design_limits.md` §5). Lockstep sends only inputs. The input rate is bounded by each
endpoint's declared capacity and does not grow with the world.

Topology: peers stamp, a server relays. Each peer picks the tick for its own commands. The
server collects the commands, merges them and broadcasts them. It never changes a tick
stamp. The server is a relay and an ordering point; it does not control time. The relay is a
*server tier* of several nodes, which also decides when a peer has dropped (§4.1).

Peers stamping their own commands has two consequences:

- Submission works the same way over the network as for a local paced producer: every
  networked command names its tick (`design_python_api.md` §7.1). The submitting peer knows the tick because it picked it
  (§3.2). If the server stamped commands, the peer would not know the tick. The rule "a
  command runs at the tick it names, or not at all" would then have nothing to refer to.
- **Every peer sends an input message for every turn, even an empty one.** The server closes
  turn `t` only when all peers have reported for it. Silence therefore means "not yet",
  never "nothing happened". The same rule applies to the server's broadcasts (§3.1).

A session admits up to 128 players (`design_limits.md` §2.1). This is a game rule. The
engine itself sizes *sources* and *participants*. It caps both at 256, so a full lobby still
leaves room for mods.

Lockstep has real costs:

| Cost | Detail |
|---|---|
| The session runs at the speed of the slowest node it waits for | Every instance must finish tick `t` before anyone runs `t+1` (§3.1). The server tier moves this cost rather than removing it. A client waits on one server node. The server nodes absorb the wait between peers, over a few reliable links instead of across 128 clients (§4.1) |
| Every peer needs the identical mod set | A Tier 3 mod is native code inside the tick. One different version causes a desync |
| Late join needs a state transfer | A joining peer must replay from tick 0 or receive a full snapshot, which is the transfer lockstep otherwise avoids |
| Every client knows the whole map | Every peer simulates everything. Hiding information is a UI feature and gives no security |

## 2. What the engine already provides

The determinism design already covers most of what lockstep needs:

| Requirement | Provided by (`design_engine_core.md` unless noted) |
|---|---|
| Bit-identical arithmetic on every platform | §2.1: fixed-point, no libm, versioned rounding and overflow rules |
| Commands as the only way to change the world | §1 |
| One wire encoding | §2.3: little-endian, fixed widths, fully validated. Replay, IPC and the network all use this one schema |
| Desync detection | §2.3, §4.2 and `design_limits.md` §6: one checksum algorithm at three levels. Peers compare the tick digest every tick and the rolling checksum, which covers the whole world every N ticks |
| A gate that waits for participants before each tick | §3.3: the core waits until every registered participant is ready for the tick, up to a declared deadline. A peer is a participant |
| A command endpoint per peer | §5.1: an endpoint is a single-producer (SPSC) ring. A network receive thread is that single producer |
| A submit call that names the tick | §5.1: every submission names its tick, so peers need no special call. The only extra thing a peer declares is a nonzero stamp margin |
| A way to report what a command did | §5.1: the outcome channel. A peer's outcomes are identical on every machine. They help spot trouble early and give the transport a way to push back, but carry no new information about the world |
| A record of what ran | §2.3 and §6: commands are recorded as they are consumed, already ordered and tick-stamped. That is exactly what a turn log must contain |
| Session identity that covers mods | §2.3 records the mod manifest list and hashes |

## 3. How lockstep maps onto the engine

### 3.1 A peer is a participant

Lockstep's core rule is that tick `t` runs only when the command set for `t` is complete on
every peer. Otherwise peers compute from different sets. The next tick digest then shows a
differing command set, or the rolling checksum shows the diverged state within N ticks. The
gate in `design_engine_core.md` §3.3 already enforces this rule for every participant, and a
peer is one more participant:

```
while (!ready_for(t))                                   // every participant, incl. peers
    if (!gate.try_acquire_until(deadline_for(t))) on_gate_timeout(t);
drain_commands(t);                                      // §5.1: per-endpoint rings
execute(t); publish(t);
first_unexecuted.store(t + 1, release);
```

At the freeze, each peer registers one participant, one endpoint and **its own receive
thread**. The dedicated thread is required. An admission wait can last up to a tick
(`design_engine_core.md` §5.1), so one thread serving all peers would stall everyone behind
whichever endpoint is full. One thread per peer also gives each endpoint the single producer
its SPSC ring requires.

For each turn, the receive thread does two things, in this order:

```
for each command in turn(t):  submit(cmd, t)            // that peer's endpoint; may wait
ready_through.store(t, seq_cst)                         // "my turn t is complete"
if (gate_waiting.load(seq_cst)) gate.release()          // wake the sim only if it is parked
```

This order is the deadlock rule of `design_engine_core.md` §5.1: submit, then release the
turn, and read outcomes only for turns already released. A thread that waited for turn `t`'s
outcomes before declaring ready for `t` would be waiting on the tick it is itself holding
up. The engine rejects that call instead of letting the session hang.

`ready_through` is the only completeness check. "Has peer P reported for turn `t`?" is
`P.ready_through >= t`, which the sim already evaluates. There is no `can_advance`, no
turn-ready structure and no second index.

#### Worked example

Three peers behind a relay, at 30 Hz, one turn per tick. Peer A is about to run tick 1000.
Peer C is the slowest.

| Wall clock | Peer A, sim thread | Peer A, network thread |
|---|---|---|
| 0 ms | finishes tick 999, `first_unexecuted = 1000` | — |
| 0 ms | `ready_for(1000)` → false (C's `ready_through` is 999); blocks on the gate | relay is still waiting on C's input for 1000 |
| 12 ms | *blocked: 12 ms of the 33 ms budget* | turn-1000 packet lands; `submit(…, 1000)` on C's endpoint; `C.ready_through = 1000`; the sim is parked, so `gate.release()` |
| 12 ms | wakes, drains, executes, publishes | sends tick 1000's tick digest and rolling-checksum slice upstream |
| 20 ms | `ready_for(1001)` → true, already buffered, so no wait | turn 1002 arrives |

A runs at C's pace. This pacing comes from the gate itself and needs no separate mechanism.

Four details make this work:

| Detail | Why it matters |
|---|---|
| An empty turn is still sent | `ready_through` cannot tell "nobody acted in turn 1000" from "turn 1000 has not arrived". So the relay sends turn `t` even when it is empty, and peers report for `t` even with nothing to submit (§1) |
| A peer's commands arrive before its readiness | So a command stamped for the tick the engine is currently waiting on is normal, not late. That is why `too_late` rejects only ticks that have already run (`design_engine_core.md` §5.1). The order shown above is the whole guarantee |
| Remote commands need no new structure | A peer is another source id with its own endpoint. Its receive thread is that endpoint's single producer, as the SPSC contract of `design_engine_core.md` §5.1 requires. The drain assigns the `(source id, sequence)` order. It is identical on every peer, because every peer drains the same per-endpoint FIFO rings in the same source order |
| The wait is bounded twice | Turns already delivered run without blocking, so a burst of three turns runs at full speed. And the participant's deadline stops a dead peer from hanging the session |

### 3.2 A peer differs only in its stamp margin

The command path in `design_engine_core.md` §5.1 has no special case for peers: every paced
submission names its tick, local or remote. A peer's endpoint differs from a mod's in a
single declared number, its **stamp margin**: how many ticks ahead of the current tick it
may stamp a command.

- The submitting peer picks tick `t` and broadcasts it, and the gate makes every other peer
  wait for `t`. So a command stamped for `t` runs at `t` on every peer, or the session
  stalls. It is never silently moved to another tick.
- `too_late` cannot happen in a healthy session. If it does, it reports a defect, not a sign
  of load: some instance ran past a turn it should have waited for. The tick the engine is
  currently waiting on is not late. It is the tick a peer's packet almost always names. It
  is safe because a peer's commands arrive before it declares ready, and the gate cannot
  advance until it does (§3.1).
- **Stamps on an endpoint must never decrease, and the engine checks this.** A submission
  that names an earlier tick than one already submitted on that endpoint is rejected at
  submission with `out_of_order`. A queued command whose tick has already passed is a hard
  protocol error, never a late execution. The engine cannot rely on peers behaving well
  here, because the failure would be invisible. If a transport reordered commands on its
  receive path, every command behind a late one would run at the wrong tick on that machine
  alone, and nothing would report it.
- The stamp margin is the classic input delay. The transport chooses it. The engine imposes
  no value and no upper limit, and needs no change to raise it.
  - The engine sizes the endpoint's ring from it. Depth is `(margin + 1) × capacity`
    (`design_limits.md` §2), because a peer with margin `m` can have `m + 1` turns in
    flight.
  - The same window is the ceiling of `HostPolicy.ipc_deadline` (`design_limits.md` §1.1).
    The worst-case totals in `design_limits.md` §2.2 also depend on it.
  - A margin of 1 is enough while the slowest peer's round-trip time (RTT) is at most one
    tick period, which is 33 ms at 30 Hz. A peer reports for turn `t+1` when tick `t`
    starts, and does not need turn `t+1` until tick `t+1` is scheduled. That covers LAN and
    much regional play.
  - Above that, `margin ≈ ceil(RTT / tick period)`. Each extra tick of margin adds one tick
    of input latency.
  - The value itself is open ([Q50](open_question.md#q50-input_delay_ticks)).
- **A mod or an engine source has a margin of 0, and the engine checks it.** It acts within
  the turn it is about to release, so it never has more than one turn in flight. Its command
  stamped for a later tick is a bug and is rejected with `over_margin`.
- **The local player submits through its own peer endpoint, with the input delay.** In
  single-player the host endpoint is unpaced: its commands name no tick, and each runs at
  the first tick that drains it (`design_engine_core.md` §3.3, §5.1). That cannot work
  across machines, which would each drain the command at a different tick. So in a
  networked session the local player's commands are stamped and paced like every other
  peer's, and carry the same source id on every machine. How that endpoint is produced, and
  what the host endpoint does in a networked session, are open
  ([Q55](open_question.md#q55-the-local-players-source-id)).

### 3.3 Pause, step and time scale apply to the whole session

Pause is a control call (`design_python_api.md` §3). It never changes core state and is
never recorded. The host controls the gate through two independent fields, `run_until` and
`stop_requested`. The sim also keeps its own message backlog count. All three feed the same
gate check as every peer's `ready_through` (`design_engine_core.md` §3.3). They are kept
separate so that one cannot overwrite another; for example, a resume cannot erase a stop or
a backlog pause.

In a network session, the session protocol applies each agreed change to the run grant
(pause, resume, step) on every instance. One peer's accepted pause therefore stalls everyone
through the normal gate rule.

**A pause stops every clock in the session, including participant deadlines.** Otherwise a
peer could time out while the game is paused, and be dropped for something it did not do
(`design_engine_core.md` §3.3).

Pause is therefore an agreement between peers that takes effect at an agreed turn. It must
not be a command: commands change core state and are recorded for replay, and pause does
neither. Pause belongs in the session protocol, next to the turn stream. The same applies to
`step()`, `set_time_scale(None)` and any other control that changes when ticks run. The
session-control protocol distributes all of them, and each instance applies them to its
local gate. They do not need to share one atomic variable.

### 3.4 Mechanisms that need no change

| Mechanism | Why it works under lockstep |
|---|---|
| Per-endpoint capacity | A declaration, identical on every peer, of how many commands one endpoint may hold for one tick. It limits what a peer may submit, never what a tick runs: the drain takes everything stamped for the tick (`design_engine_core.md` §5.1). So there is no per-tick cap that two peers could apply to different amounts of buffered input |
| `(source id, sequence)` total order | The drain assigns it from data every peer has: endpoint order and position in the ring. Nothing is sent over the wire that could be forged, and nothing is sorted |
| Replay | A lockstep session's turn log is a replay artifact. Commands are recorded as they are consumed, so the two are the same data in the same format |
| Engine views, projection, cadence | Entirely local. No engine view data crosses the network, and a peer's engine view settings cannot change what it computes |

## 4. Authority

Most commands need no authority to decide them. `design_engine_core.md` §2 requires a single
authority only for commands from non-deterministic peripherals:

| Command source | Needs an authority? | Why |
|---|---|---|
| Player input (every player's peer endpoint, the local player's included) | No | The gate is enough. The submitting peer names the tick and broadcasts it, and every other peer waits for that tick |
| Deterministic core mods, and agent minds inside the determinism boundary | No | Every peer computes the identical command from identical state (`design_engine_core.md` §6) |
| Non-deterministic peripherals: float, GPU or LLM-based minds outside the boundary | Yes | Peers would compute *different* commands. Either one peer computes and broadcasts them, or the mind moves inside the boundary |

So commands do not need a server. They need a rule for the third row: move the mind inside
the boundary, or pick one peer to own it. `design_engine_core.md` §6 describes both options.
Which one, and whether per subsystem or globally, is open
([Q53](open_question.md#q53-non-deterministic-peripherals-in-multiplayer)).

Source ids must be identical on every peer. They are `u32` values assigned at the freeze,
with 0 reserved for the host. Session formation assigns them, and they become part of
session identity, like the mod manifest hashes. The participant and endpoint sets close at
the same freeze step, so a peer that joins later starts a new session (§1). The local
player's own commands use that player's peer endpoint, so they carry the same source id on
every machine (§3.2). How that endpoint is produced is open
([Q55](open_question.md#q55-the-local-players-source-id)).

### 4.1 Two network tiers

Deciding that a peer is gone is a different kind of authority from deciding commands. It is
the one place this design needs distinguished nodes.

| Tier | What it is | What it may decide |
|---|---|---|
| Server nodes | A small set of nodes that agree among themselves over links treated as reliable | That a peer has dropped, and the turn at which the drop takes effect |
| Client nodes | Everyone else. Each client receives from one server node | Nothing about liveness. A client reports an expired deadline upward and waits |

Two rules follow.

**Only the server tier may decide that a peer has dropped.** A peer's commands are part of
the command set for each turn. An instance that removed a peer on its own timer would run a
different command set from everyone else, and desync (`design_engine_core.md` §3.3,
`on_expiry = DROP`). So a client's participant deadline only *reports*. Until the server
tier answers, the client pauses and says why. It never continues without the peer.

**A drop takes effect at an agreed turn.** Removing a peer changes the command set, so the
drop travels in the turn stream like any other change and applies at a turn every instance
can name. Two machines applying the same drop at different turns would diverge, just as with
local timeouts. The tiers settle who decides; the agreed turn prevents the desync. Both are
needed.

The tiers also decide where lockstep's waiting cost is paid. Waiting for the slowest player
cannot be avoided (§1). With tiers, that wait happens among a few server nodes on reliable
links, instead of across 128 clients on the open internet. Each client waits on one server.

## 5. One turn is one tick

Classic RTS lockstep uses turns that span several ticks. This lowers the packet rate, but
the main benefit is that a long turn hides one network round trip. This design uses one turn
per tick instead:

| | turn == tick | turn spans K ticks |
|---|---|---|
| Fits the existing per-tick gate and drain | Yes, unchanged: `ready_through` is already a tick number | No: needs a second index and a rule for spreading a turn's commands over its ticks |
| Packets per second per peer | 30 at 30 Hz | 5–10 |
| Input granularity | one tick | one turn |

The gate and the drain already work per tick. A rate of 30 packets per second per peer is
modest for the peer counts lockstep supports. If the packet rate ever matters, the transport
can batch turns without changing the model.

Raising the input delay (§3.2) hides a round trip just as a long turn does. Both cost about
one round trip of input latency, and no scheme can do better. One turn per tick is better on
two counts. The input delay can adapt to the measured round trip at runtime, while turn
length is fixed in the protocol. And no second index or spreading rule is needed. At 30 Hz,
a margin of 1 already covers round trips up to 33 ms (§3.2).

## 6. What M1 must preserve

M1 builds everything lockstep needs for single-player reasons, so lockstep adds no work to
M1:

| Lockstep needs | M1 builds |
|---|---|
| A gate on tick advance | `design_engine_core.md` §3.3. In single-player it has no participant: the host is unpaced |
| A command endpoint per peer | the per-endpoint SPSC ring of `design_engine_core.md` §5.1 |
| A submit call that names the tick | the paced submit of `design_engine_core.md` §5.1, which mods use for the same reason peers do. The host's single-player submit names no tick, and a networked session does not run the local player through it (§3.2) |
| Session-wide pause and step | independent host-control fields feeding the gate check (`design_engine_core.md` §3.3). The session protocol distributes the agreed change |

In single-player, `run_until = U64_MAX`, `stop_requested = 0`, the message backlog is below
its high-water mark, and there is no participant: the host is unpaced. The gate check then
costs two loads and one comparison per tick, and it blocks only on a pause, the end of a
step or the backlog (`design_engine_core.md` §3.3).

M1 must keep two rules:

- **Do not assume "local" in the submit API.** A command carries a source id, and its
  endpoint declares a stamp margin. Nothing in the M1 API may assume the submitter runs in
  the same process.
- **Keep pause, resume, step, stop and backlog independent.** They combine only in the gate
  check, so none can overwrite another. The backlog is not shared state at all
  (`design_engine_core.md` §5.2).

## 7. Open questions

These are tracked in [open_question.md](open_question.md). None of them blocks M1.

- [Q48](open_question.md#q48-stall-policy) Stall policy: timeouts, drops, and what a dropped
  peer's absence means for the command stream. §4.1 fixes the shape; the details need a
  transport.
- [Q49](open_question.md#q49-peer-deadline-and-drop-escalation) The peer deadline value, and
  the escalation protocol behind `on_expiry = DROP`.
- [Q50](open_question.md#q50-input_delay_ticks) `input_delay_ticks`, the value of a peer's
  stamp margin (§3.2).
- [Q51](open_question.md#q51-transport-and-session-formation) Transport, session formation,
  NAT traversal, rejoin and late join.
- [Q52](open_question.md#q52-checksum-comparison-between-peers) How often peers compare
  checksums, and what a detected desync does.
- [Q53](open_question.md#q53-non-deterministic-peripherals-in-multiplayer) Non-deterministic
  peripherals: elect an owning peer, or require the mind inside the boundary (§4).
- [Q54](open_question.md#q54-enforcing-the-same-mod-set-on-every-peer) Enforcing an
  identical mod set between peers. Session identity records the mod set but does not compare
  it.

---

## References

- `design_engine_core.md` §1 (command path), §2 (determinism), §2.1 (arithmetic), §2.3
  (identity and recording), §3.3 (participants and the gate), §5.1 (command ring,
  tick-naming submission, stamp margins), §5.2 (event ring and backlog), §6 (minds inside
  and outside the boundary)
- `design_python_api.md` §3 (control calls), §4.3 (threading phases, catch-up clamp), §7.1
  (submission results)
- `design_limits.md` §2 (endpoint capacity, ring depth), §2.1 (players, sources,
  participants), §5 (why state replication does not scale), §6 (checksum), §7 (input delay)
- [Gaffer On Games — Deterministic
  Lockstep](https://gafferongames.com/post/deterministic_lockstep/)
- [SnapNet — Netcode Architectures Part 1:
  Lockstep](https://www.snapnet.dev/blog/netcode-architectures-part-1-lockstep/)
