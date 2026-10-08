# Litmus tests

These tests check the lease's ordering argument with herd7 (`design_engine_core.md` §3.3,
§5.1). Neither ThreadSanitizer nor an x86 machine can check it. x86 never runs the bad
execution, and ThreadSanitizer checks only the executions the machine runs. The build does
not run these tests.

## The lease

Two leases share one protocol: the endpoint lease (`design_engine_core.md` §5.1) and the
operation lease (`design_python_api.md` §6). A holder adds to a count, then loads the
state. The waiter changes the state, then reads the count with a read-modify-write.

In every test, `x` is the count (`active_submit`, `active_operations`) and `y` is the
state: 0 for `OPEN`, 1 for closing (`REVOKING`, `CLOSING`). `P0` is a holder, a submit or
a native call. The last thread is the waiter, `revoke` or `close()`.

## Expected verdicts

"Never" means the model forbids the bad outcome. A control must say "Sometimes": it shows
that the test can see the hole the read-modify-write closes.

| Test | What it checks | Expected |
|---|---|---|
| `LEASE-enter-rmw` | entering against the waiter, as written | Never |
| `LEASE-leave-rmw` | leaving against the waiter: a lost wake-up | Never |
| `LEASE-leave3-rmw` | two holders; the waiter reaches the last through the other's subtraction | Never |
| `LEASE-enter-rmw-minimal` | the weakest orders that hold: the waiter's read-modify-write `release`, the holder's add `acquire` | Never |
| `LEASE-enter-load` | control: the waiter reads the count with a plain acquire load | Sometimes |
| `LEASE-leave-load` | control: the same, for leaving | Sometimes |
| `LEASE-enter-rmw-relaxed` | control: both read-modify-writes relaxed | Sometimes |

## Running them

herd7 is part of [herdtools7](https://github.com/herd/herdtools7). Install it through opam,
then run every test under RC11, the repaired C11 model of Lahav et al., "Repairing
Sequential Consistency in C/C++11" (PLDI 2017):

```bash
opam install herdtools7
herd7 -model rc11.cat test/litmus/*.litmus
```

Each test prints a line `Observation <test> Never|Sometimes ...`. Compare it with the table.

## Keeping them current

A test models the code; it is not the code. A change to a memory order in `try_enter`,
`leave` or `revoke` (`engine.endpoint.cpp`), or in the operation lease, changes the
matching test in the same commit.
