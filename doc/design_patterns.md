# Design Patterns — Lifecycle, Construction, Init/Deinit

Reference for the recurring patterns in the current codebase (`log`, `gpu`,
`viz` subsystems). New subsystems should follow these conventions. A deviation
needs a documented reason.

Sources of truth:

- `src/libsim_estab/module/sim_estab--log.cppm` + `src/libsim_estab/src/log.cpp`
- `src/libsim_estab/module/sim_estab--gpu.cppm` + `src/libsim_estab/src/gpu.cpp`
- `src/libsim_estab/module/sim_estab.viz/viz.cppm` + `src/libsim_estab/src/viz/viz.cpp`
- `src/sim_estab_ext/bind.cpp` (Python bindings)

How to read this doc:

- Long sections start with a **TL;DR**.
- The lifecycle sections (§2–§4) are split into **Pattern** and **Example**.
- **Pattern** is the generic contract. Copy these rules as-is for any new
  subsystem.
- **Example** shows how the current code instantiates the pattern.
- Example-only constraints are facts about the wrapped resource (SDL,
  Boost.Log). They are *not* part of the pattern.
- For a new resource, discover its own constraints. Think: thread affinity,
  per-process uniqueness, immutable creation options.

---

## 1. Module organization

**TL;DR**

- One primary module `sim_estab`, with one partition per subsystem.
- Heavy optional subsystems get a separate module (`sim_estab.viz`).
- A subsystem's internals go in a non-exported partition.
- Every file's name and folder follow from the module or partition it holds.
- Macros stay in ordinary headers. Foreign types cross the interface as
  opaque `void *` aliases.

**Rules**

- One primary named module: `sim_estab`.
- Subsystems are **module partitions**: `sim_estab:log`, `sim_estab:gpu`,
  `sim_estab:util`.
- `sim_estab.cppm` re-exports them (`export import :log;`).
- A subsystem's internals that importers must not see go in a
  **non-exported partition**, declared without `export`
  (`module sim_estab:<name>;`). Every unit of the module can import it. An
  importer of `sim_estab` cannot, and `sim_estab.cppm` cannot re-export it.
- Optional/heavy subsystems that pull extra dependencies live in a
  **separate module** (`sim_estab.viz`). That module does `import sim_estab;`.
- Namespaces follow the modules. The primary module is the core, the program
  that runs without a window, so a partition's namespace is
  `sim_estab::core::<partition>`: `sim_estab::core::log`, `sim_estab::core::gpu`.
  A separate module has its own namespace outside `core`: `sim_estab::viz`.
- Not every peripheral is in the core. A peripheral may have its own module:
  viz has one, and the AI and modding parts may get one.
- Small cross-cutting helpers go in the `:util` partition. They live in
  sub-namespaces of `sim_estab::core::util`. Example:
  `util::thread_util::is_main_thread()`.
- An interface unit holds declarations, doc comments, and small
  inline/template code. Its global module fragment includes standard headers
  only.
- An implementation unit in `src/` declares the module, never a partition:
  `module sim_estab;` without `export`. A non-exported partition is not one
  of these; it lives in `module/` (see the file layout below).
- Implementation units include the heavy third-party headers (Boost.Log,
  SDL3). They do so in their **global module fragment** only.
- Modules cannot export macros. So macros stay in ordinary headers under
  `include/` (see the file layout below).
- Never forward-declare foreign struct types (e.g. `SDL_GPUDevice`) in a
  module interface. Module type mangling makes them incompatible with the
  header-defined type.
- Instead, expose them as opaque `void *` aliases (`SDL_GPUDevice_ptr`,
  `ComputeBufferHandle`). `static_cast` in the implementation unit.

### File layout

A file's path follows from the module or partition it holds, so a reader can
find the file from its `import`. Each module has a folder and a **short
name**: the last component of the module name.

- **Module folders.** Each module's interface units sit in its own folder.
  - The primary module `sim_estab` uses `module/` itself. Its short name is
    `sim_estab`.
  - A secondary module `sim_estab.<m>` uses `module/sim_estab.<m>/`. Its short
    name is `<m>`.
- **Interface units.**
  - A module's primary interface is `<short>.cppm` in its folder:
    `module/sim_estab.cppm`, `module/sim_estab.viz/viz.cppm`.
  - A partition `<p>` is `<short>--<p>.cppm` in the same folder, with `--`
    standing for `:`: `module/sim_estab--log.cppm`.
  - A non-exported partition is named the same way and also lives in
    `module/`.
- **Implementation units.**
  - A partition `<p>` of `sim_estab` is implemented in `src/<p>.cpp`
    (`src/log.cpp`, `src/gpu.cpp`).
  - A partition with only inline and `constexpr` content has no
    implementation unit (`:limits`, `:error`).
  - A partition made of independent helpers, each in its own sub-namespace,
    gets a folder with one file per helper: `src/<p>/<sub>.cpp`
    (`src/util/thread_util.cpp`). A partition that needs several files uses
    the same form.
  - A secondary module's units go in `src/<m>/`, under its short name. The
    primary interface is implemented in `src/<m>/<m>.cpp` (`src/viz/viz.cpp`),
    and a partition in `src/<m>/<p>.cpp`.
- **Folder names in `src/`.** A folder in `src/` is either a partition or a
  secondary module. So **a secondary module's short name must differ from
  every partition name of `sim_estab`.**
- **Headers.** `include/` holds only what a module cannot export: macros, and
  the standard headers a unit includes before it imports or implements the
  module. Headers are per module, not per partition.
  - `macro.h` holds the macros (`SIM_ESTAB_LOG_*`).
  - `compat.h` lists the standard headers. This hybrid exists because g++
    header export support is incomplete.
  - The primary module's headers sit in `include/`, and a secondary module's in
    `include/<m>/` (`include/viz/compat.h`).
- **Include guards.** A guard is `LIBSIM_ESTAB__` plus the path under
  `include/`, in upper case, with `/` written `__` and `.h` written `_H`:
  `LIBSIM_ESTAB__COMPAT_H`, `LIBSIM_ESTAB__VIZ__COMPAT_H`.
- **Include paths.** These differ inside and outside the project.
  - Code inside the project, tests included, writes the path relative to
    `include/`: `<compat.h>`, `"viz/compat.h"`.
  - The install puts `include/` at `include/sim_estab/`. So code built against
    the installed package writes `<sim_estab/...>`: consumers do
    `import sim_estab;` + `#include <sim_estab/macro.h>`, and `bind.cpp`
    includes `<sim_estab/compat.h>`.
- **Folder names differ between trees.** A secondary module's folder is named
  by the full module name in `module/` (`sim_estab.viz/`), and by the short
  name in `src/` and `include/` (`viz/`).

## 2. Subsystem lifecycle: `X_init` / `X_deinit` / `X_is_init` triad

**TL;DR**

- For process-global subsystems that initialize once.
- Three functions: throwing idempotent `init`, noexcept idempotent `deinit`,
  noexcept `is_init` query.
- One global mutex per subsystem guards its mutable state. The `is_init` query prefers to be
  lock-free: an atomic load (see the lock hierarchy below). Where a mutex already guards the
  state the query reads, the query may lock that mutex.

### Pattern

Use for process-global subsystems. They initialize once, not per object.

```cpp
export void X_init();                 // may throw; idempotent (second call is a no-op)
export void X_deinit() noexcept;      // idempotent, exception-safe, allows re-init afterwards
export [[nodiscard]] bool X_is_init() noexcept;  // pure query
```

Implementation rules:

- All mutable state lives in a `namespace detail { ... }` inside the
  implementation unit. That state is:
  - one `static std::mutex`;
  - a `static std::atomic<bool> init_status`;
  - whatever tracking containers the subsystem needs.
- Use a **single global mutex per subsystem**. Take it as the first line of every function
  that touches mutable state.
- **`X_is_init()` prefers to be lock-free.** Make it one acquire load of an atomic, such as
  `init_status`. A lock-free query cannot deadlock against a caller that already holds the
  subsystem mutex, and it never queues its callers on one lock.
- **Where a mutex already guards the state the query reads, the query may lock that mutex.**
  No separate atomic is needed then. The refcount pattern of §3 works this way: its mutex
  guards the count, and its query locks the mutex to read it. Such a query must not be
  called by code that already holds the same mutex.
- **For logging, a lock-free query is required.** The logging guard runs on every emission
  in the process. If it took the log mutex, `log_deinit` would deadlock, because it emits
  its last record while holding that mutex. And every emission in the process, including
  those inside the parallel engine core update, would queue on one lock. A lock-free query
  is what makes the lock hierarchy below possible.
- **`is_init` is advisory.** Its answer can be stale as soon as it returns. It may gate a
  best-effort action, such as emitting a record or skipping a toggle. It must never gate a
  decision that has to stay true after the call. Anything that needs such a decision takes
  the lock and checks again under it.
- `init` behavior, in order:
  1. Check `init_status`. Return early if already set.
  2. Do a defensive cleanup of any stale state.
  3. Build fresh state.
  4. Store `init_status = true` (release) **last**, so a hot-path caller that sees it also
     sees the fully built state.
- `deinit` behavior, in order. The flag moves **first**:
  1. Store `init_status = false` (release), so no new work starts against state that is
     about to be torn down. Storing it last would leave exactly that window open.
  2. Flush pending work.
  3. Tear down in reverse order, leaving the subsystem ready for another `init`.
- **The caller must not call `deinit` while other threads still use the subsystem.** The
  flag is a backstop, not a barrier: work already past the check may be dropped. `deinit`
  does not wait for in-flight callers, because that would put an unbounded wait inside a
  `noexcept` teardown. For logging, the engine's shutdown order satisfies this: it stops mod
  hosts and joins the sim thread before releasing logging (`design_python_api.md` §2).
- **Dropping work at teardown is acceptable only if it is safe, and for logging this has
  been checked.** The lock-free query does not create the check-then-emit window. A locked
  query would release the lock before the record was pushed anyway, because emitting spans
  building a logger, opening a record, streaming and flushing. What the flag decides is the
  arrival window, and it decides it in the caller's favour: an emitter arriving during
  `deinit` sees the flag already cleared and skips at once, instead of blocking through the
  whole teardown and then skipping.

  The remaining window is safe because of Boost.Log, not our mutex. `open_record` captures
  **weak** pointers to the accepting sinks under the core's read lock. `push_record` locks
  each one and skips any that fail. `remove_all_sinks()` takes the core's write lock and
  drops the core's own strong references, so a sink is destroyed only when its last strong
  reference goes, and an in-flight push holds one. An emitter racing teardown therefore
  either finds the sink gone and skips it, or keeps it alive until `consume()` finishes.
  **It is never a use-after-free, in either order.**

  The general rule: a subsystem may drop work at teardown only if its teardown is safe
  against a caller already past the guard. Otherwise it needs a drain, and cannot use this
  pattern.
- `deinit` is `noexcept`. It is safe to call when never initialized.
- Optional feature toggles follow the same shape: `enable_Y()` /
  `disable_Y() noexcept`. Both are idempotent. Both are no-ops when the
  subsystem is not initialized.
- Hot-path helpers guard themselves. They return silently when
  `!X_is_init()` instead of throwing.

### Example: logging (`sim_estab:log`)

- Tracking container: `flat_map<sink_type, sink_ptr> sink_map`. It tracks
  the sinks registered with the Boost.Log core.
- Defensive cleanup in `log_init`: flush, clear the tracking map, remove
  stale sinks from the core.
- Feature toggles: `enable_console()` / `disable_console()`. The file sink slots exist in
  the enumeration but have **no toggle pair yet**. They are planned (`design_logging.md`
  §3), and this document must not describe them as shipped.
- Hot-path guard: `sim_estab_log()` returns silently when `!log_is_init()`. That is one
  atomic load, with no lock, on every emission in the process.
- **The subsystem's own lifecycle records are ordered around its lock**, which is the one
  place §7 needs care. `log_init` emits its `info` record after releasing the lock and
  setting the flag; before that, the guard would drop it. `log_deinit` emits its record
  while still holding the lock, before clearing the flag and tearing down. The log subsystem
  logs its own lifecycle like every other subsystem; only the order is special.

### Lock hierarchy

§3 requires logging refcount changes while holding a subsystem mutex, and that is correct.
It needs one rule:

> **Logging is the bottom of the hierarchy.** Any subsystem may emit while
> holding its own lock. Nothing reachable from the emission path may acquire a
> lock that anything else takes above logging.

This makes `subsystem_mutex → log` a documented lock order rather than an undocumented
nesting. It is also the constraint any future file or network sink must meet: a sink called
from the log path may hold its own lock and no other. Two consequences, because this is
where the rule gets broken:

- **The log subsystem never calls into another subsystem**: not for formatting,
  configuration or diagnostics.
- **`log_is_init()` is lock-free so the hierarchy has a bottom.** If the query took the log
  mutex, `log_deinit`, which emits while holding it, would deadlock against itself. And
  every emission under any subsystem lock would serialize the whole process on one mutex.

Within a subsystem, a second mutex is allowed only if its order is stated where both are
declared. `gpu` has two, acquired as `GPU_device_mutex` → `SDL_ctx_mutex` and never the
reverse.

## 3. Ref-counted shared globals: `acquire` / `release` pair

**TL;DR**

- For one process-wide resource shared by many independent objects.
- First acquire creates. Last release destroys.
- `acquire` may throw. `release` is noexcept and tolerant.
- Immutable creation options: first caller wins. Capabilities: accumulate.
- Thread affinity, if any, is enforced on the first acquire.

### Pattern

Use when several independent objects must share one process-wide resource.
Typical cause: the underlying library allows only one instance per process.

```cpp
export void X_acquire(/* capabilities, creation options */);   // throws on failure
export void X_release() noexcept;                              // no-op if never acquired
export [[nodiscard]] bool X_is_init() noexcept;
export [[nodiscard]] X_handle X_get() noexcept;  // if the resource is handle-shaped;
                                                 // nullptr if not initialized
```

Implementation rules:

- The `detail` namespace holds:
  - a `static std::mutex`;
  - a `static int ref_count`;
  - the resource pointer/flags.
- Every function locks the mutex first. That includes `X_is_init()`: the
  mutex already guards the count, so the query locks it and reads the count
  (§2).
- **First acquire creates. Last release destroys.**
- `release` decrements. It tears down only at zero. Calling `release` with
  `ref_count <= 0` is a safe no-op.
- `acquire` may throw the subsystem error. On the failure path, it first
  restores all `detail` state to the "never created" values.
- Parameters to `acquire` come in two kinds:
  - **Accumulable capabilities.** Later acquires add them additively.
    Nothing is shut down until the refcount reaches zero.
  - **Immutable creation options.** These cannot change after creation.
    First caller wins. A later mismatching request logs a `warning` and is
    ignored.
- Enforce layering/preconditions with a thrown error. No implicit auto-init.
  If a required dependency is not initialized, throw. The *caller* (context
  object) acquires dependencies in order.
- If the resource has **thread affinity**, enforce it on the *first* acquire:
  - Throw when `ref_count == 0` and the current thread is not the required
    one.
  - Check via `core::util::thread_util::is_main_thread()` from the
    `sim_estab:util` partition.
  - Once a reference exists, worker threads may bump and release freely.
  - Recommended usage: acquire an app-lifetime reference on the required
    thread at startup. Then the refcount never returns to zero mid-run.
    Otherwise, the next first-acquire re-runs the affinity check on whatever
    thread gets there first.
- Log every refcount change at `debug`, with the new count, **inside the lock**. The §2 lock
  hierarchy allows this: logging sits below every subsystem, so `subsystem_mutex → log` is
  an order, not a cycle. Log create and destroy at `info`.

### Example: SDL context and shared GPU device (`sim_estab:gpu`)

```cpp
export void SDL_ctx_acquire(SDL_InitFlags subsystems = 0);   // throws on failure
export void SDL_ctx_release() noexcept;                      // no-op if never acquired
export [[nodiscard]] bool SDL_ctx_is_init() noexcept;

export [[nodiscard]] SDL_GPUDevice_ptr GPU_device_acquire(bool debug = false, bool low_power = false);
export void GPU_device_release() noexcept;
export [[nodiscard]] SDL_GPUDevice_ptr GPU_device_get() noexcept; // nullptr if not initialized
export [[nodiscard]] bool GPU_device_is_init() noexcept;
```

The points below are facts about SDL. This instance of the pattern has to
encode them. Discover the equivalents for any new resource:

- SDL3 GPU does not support multiple devices per process. So the device is a
  ref-counted shared global, not a per-context object.
- Accumulable capability: the `subsystems` init flags. Later acquires
  initialize additional SDL subsystems additively.
- Immutable creation options: GPU `debug` mode and `low_power` preference.
  Later mismatches warn and are ignored. On the failure path,
  `GPU_device_acquire` resets `debug_mode`/`prefer_low_power` before
  throwing.
- Layering: `GPU_device_acquire` throws `gpu_error` if
  `SDL_ctx_is_init()` is false.
- Init queries: `SDL_ctx_is_init()` and `GPU_device_is_init()` lock their
  mutex and read the count. `GPU_device_acquire` calls `SDL_ctx_is_init()`
  while it holds `GPU_device_mutex`. That nesting is the lock order stated in
  §2.
- Thread affinity: SDL's video/main thread is whichever thread first
  initializes video. On Apple platforms, that must be the real main thread.
  So `SDL_ctx_acquire` throws `gpu_error` when `ref_count == 0` off the main
  thread. Hence: acquire an app-lifetime reference on the main thread at
  startup.

## 4. RAII context objects

**TL;DR**

- Per-instance handle. The ctor acquires everything. The dtor releases
  everything.
- Explicit throwing ctor, noexcept dtor, deleted copy and move.
- Cleanup runs in the exact reverse order of construction.
- Cleanup releases only what was provably acquired.

This is the public face over the acquire/release layer (§3).

### Pattern

Interface rules:

- The constructor is `explicit`. It takes user-facing options with defaults.
  It throws the subsystem error on any failure.
- The destructor is `noexcept`.
- **Non-copyable and non-movable.** All four special members are `= delete`.
  Reason: the wrapped resources usually cannot move safely, and the SBO
  pimpl pins the address.
- Queries are `const`. Mark them `[[nodiscard]]` where the result is the
  whole point.

Construction order (cleanup is the exact reverse):

```
ctor: placement-new Impl  →  acquire shared globals in dependency order (§3)
      →  create owned per-instance resources  →  log
dtor: Impl::~Impl → cleanup(): destroy owned resources in reverse order
      →  release shared globals in reverse order
```

Implementation rules:

- All state lives in `struct Impl`, defined only in the .cpp.
- `Impl` is itself non-copyable and non-movable.
- `~Impl() { cleanup(); }`. The `void cleanup() noexcept` releases in
  **reverse order of creation**. It nulls/clears state as it goes.
- Mark shared resources "not owned" in comments. Release them via the
  ref-count API. Never destroy them directly.
- **cleanup() releases only what was provably acquired.**
  - Every acquisition is witnessed by Impl state. A witness is a non-null
    pointer, or a boolean flag set right after the acquiring call succeeds.
  - Each release in `cleanup()` is guarded by its witness.
  - Never release a ref-counted global unconditionally. If the acquire
    threw, that would steal a reference held by another live context.
- **Disarming: the one case where `cleanup()` must not release.** Release in reverse order
  is unconditional, except when releasing would be provably unsafe. If a thread could not be
  stopped, it may still be using the resource, and freeing it would be a use-after-free that
  "always clean up" would require. So a context may be **disarmed**:

  ```cpp
  void cleanup() noexcept {
      if (disarmed_) {                  // clear witnesses, release NOTHING
          log_critical(...);            // name every resource being leaked
          null_all_witnesses();
          return;
      }
      ... normal reverse-order release ...
  }
  ```

  - Disarming is narrow. The only valid reason is "a live thread outside our control may
    still reach this". It is never a fallback for "cleanup looked risky" or "an error
    happened".
  - It is never silent. A disarm logs `critical` and names what leaked. Leaking quietly is
    worse than crashing.
  - **Do not release ref-counted globals on a disarmed path** (§3). Decrementing would let a
    later acquire hand a live device to a process that has lost track of one. As a result
    the process can never acquire that subsystem again, and that is intended: the process is
    poisoned and should stay so.
  - Disarming can apply to a single resource, where it is usually called **detaching**. One
    block of memory is handed to whoever still references it and freed on their schedule,
    while the rest of the context releases normally. An engine view's block detach at close
    (`design_engine_core.md` §3.1) works this way.
  - The Python face of a disarmed context is the terminal `failed` engine state
    (`design_python_api.md` §2).

- **Constructor exception safety**: if any step throws, destroy the
  partially-built Impl. Null the cached pointer. Then rethrow. The
  destructor later sees `impl_ == nullptr` and does nothing:

  ```cpp
  try {
      ... acquire steps ...
  } catch (...) {
      impl_->~Impl();   // cleanup() releases whatever was acquired so far
      impl_ = nullptr;
      throw;
  }
  ```

  - Wrap the **whole constructor body** in one `try { ... } catch (...)`.
    That means everything after the placement-new, including trailing
    logging.
  - Use `catch (...)`, not a specific exception type. Then `std::bad_alloc`
    and friends also trigger rollback.
  - Boolean flags in Impl record sub-steps that need distinct undo actions.
- Translate cross-subsystem errors **at the boundary**:
  - Catch the dependency subsystem's error type.
  - Rethrow as this subsystem's error.
  - Prepend context to the original message.
- Decide debug/release divergence at compile time in the ctor
  (`#ifdef NDEBUG`).

### Example: `ComputeContext`, `VizContext`

Concrete construction/cleanup chain:

```
ctor: placement-new Impl  →  SDL_ctx_acquire(subsystems)  →  GPU_device_acquire(...)
      →  create owned resources (window, claim window for device)  →  log backend info
dtor: Impl::~Impl → cleanup(): release window claim → destroy window
      → GPU_device_release() → SDL_ctx_release()
```

- `[[nodiscard]]` queries: `has_device()`, `device_info()`.
- Acquisition witnesses: pointers `gpu_device`, `window`; flags
  `sdl_ctx_acquired`, `window_claimed`.
- Error translation: `VizContext` catches `gpu_error`. It rethrows as
  `viz_error` (`"SDL initialization failed: " + e.what()`).
- Compile-time divergence: GPU validation layers are on in debug builds
  only.

## 5. Fast pimpl (SBO) — two variants

**TL;DR**

- No heap allocation for pimpl. The Impl is placement-new'd into an aligned
  in-object buffer.
- Variant 5a: context objects. Variant 5b: opaque 1:1 wrappers.

### 5a. Context style (`ComputeContext`, `VizContext`)

```cpp
private:
    struct Impl;                                   // defined in .cpp
    static constexpr size_t impl_size  = 512;      // headroom, not exact
    static constexpr size_t impl_align = alignof(void *);

    alignas(impl_align) unsigned char impl_buffer_[impl_size];
    Impl *impl_ = nullptr;                         // cached; nullptr = not constructed

    void check_impl() const noexcept;              // aborts on null, even in release
```

- Put `static_assert(sizeof(Impl) <= impl_size)` (and the `alignof` check)
  **inside the constructor**. That is the only place where Impl is complete.
- The cached `impl_` pointer avoids repeated `std::launder`.
- `impl_` doubles as the "constructed" flag. The destructor does:
  `if (impl_) { impl_->~Impl(); impl_ = nullptr; }`.
- **Mutating operations** call `check_impl()` first. On null, it logs
  `critical` ("programming error") and calls `std::abort()`.
- **Read-only queries degrade gracefully instead**: `if (!impl_) return {};`. This is the
  query contract, not an exception to §6; §6 states the same split from the other side. The
  reason is not leniency. Reporting what a context was, after something went wrong, is
  exactly when a getter gets called on a destroyed or never-constructed object. A getter
  that aborted there would kill the process and destroy the diagnostic the caller was
  building. A query returns a value-initialized result, which every caller must already
  handle, because a live but empty context can return the same.
- **Both contexts follow it**, and the rule is stated once here so they cannot drift apart.
  `device_info()` degrades on both contexts, and so do the other `VizContext` queries.

### 5b. Opaque wrapper style (`record`, `record_ostream`, `logger`, `logger_mt`)

For 1:1 wrappers that completely hide a third-party type (Boost.Log) from
the module interface:

```cpp
public:
    // public so the impl file can static_assert against the real type
    static constexpr std::size_t storage_size  = 256;
    static constexpr std::size_t storage_align = 16;
private:
    alignas(storage_align) std::byte storage_[storage_size];
    void *get_impl() noexcept;               // == static_cast<void*>(storage_)
    const void *get_impl() const noexcept;
```

- The .cpp has a file-scope
  `static_assert(sizeof(wrapped) <= wrapper::storage_size, ...)`. Same for
  alignment.
- Every member is a thin forwarder: `static_cast<Wrapped*>(get_impl())`,
  then call through.
- Constructors placement-new. The destructor does
  `std::launder(reinterpret_cast<Wrapped*>(storage_))` + `std::destroy_at`.
- **Copy/move semantics mirror the wrapped type exactly**:
  - `record` is move-only.
  - `record_ostream` is neither copyable nor movable.
  - `logger[_mt]` is fully copyable and movable.
- Copy assignment uses copy-and-swap. Each copyable/swappable wrapper also
  gets a free `swap()` overload.
- Friendship (`friend class logger;` etc.) grants the collaborating wrappers
  access to `get_impl()`. Do not expose it otherwise.

## 6. Error handling conventions

**TL;DR**

- One exception family: the root `sim_estab_error`, and one error per
  subsystem under it.
- Create/acquire may throw. Release/destroy/query never throw.
- Log the failure before throwing.
- Programming errors abort. They do not throw.

Rules:

- One exception family for the whole library. Its root is `sim_estab_error`,
  in the `:error` partition (`sim_estab::core::error`). It derives from
  `std::runtime_error`. One `catch` clause for the root covers every error the
  library throws. Nothing throws the root itself.
- One error per subsystem. Export it from the module. Derive it from the root.
  Use the single canonical constructor:

  ```cpp
  export class x_error : public error::sim_estab_error {
  public:
      explicit x_error(const std::string& what) : error::sim_estab_error(what) {}
  };
  ```

- A subsystem whose callers must tell its errors apart derives one class per
  case from its own error. A caller then selects by type and never reads a
  message. The engine's errors follow this: one C++ class for each error type
  of `design_python_api.md` §8 that native code raises. A type that Python
  raises before it reaches native code has no C++ class.

- Direction of throw/noexcept:
  - Anything that **creates/acquires** may throw.
  - Anything that **releases/destroys/queries** is `noexcept`. It tolerates
    the never-initialized state.
- On a C-API failure:
  1. Capture `SDL_GetError()` immediately.
  2. Log at `error`.
  3. Throw with `"<what failed>: " + error`.
  - Clean up any locals acquired within the function on every early exit
    path (transfer buffers, command buffers, properties). That includes
    `SDL_CancelGPUCommandBuffer` for unsubmitted command buffers.
- Validate arguments up front. Use specific messages:
  `"Cannot create buffer with size 0"`, `"Cannot dispatch with null pipeline"`.
- Benign no-ops return early instead of throwing. Examples: empty upload
  span, zero-size download.
- Use-after-destruction or uninitialized access **through a mutating operation** is a
  **programming error**: `check_impl()` → log `critical` → `std::abort()`. Never an
  exception. The rule covers mutating operations only, so it is not read as "every access
  aborts":
  - **Mutating operations abort.** Continuing past one either corrupts state or does nothing
    while the caller believes it worked, and no return value can say "your object is gone".
  - **Read-only queries degrade**: `if (!impl_) return {};` (§5a). They do not abort,
    because the caller is most likely a diagnostic reporting the very failure that destroyed
    the object.
  - The Python surface goes further: use after close raises `EngineClosedError` instead of
    aborting (`design_python_api.md` §2).

## 7. Logging conventions

- Channels are hierarchical: `"sim_estab.<subsystem>"` (`sim_estab.gpu`,
  `sim_estab.viz`). Components may extend: `module.subsystem.component`.
- Severity usage:
  - `info`: lifecycle create/destroy.
  - `debug`: ref-count ticks, successful sub-steps, per-op traces.
  - `warning`: recoverable/ignored mismatches.
  - `error`: failures that throw.
  - `critical`: abort-level programming errors.
- Always log the failure **before** throwing.
- In-library code calls `sim_estab_log(channel, severity_level::x, parts...)`
  directly. It is variadic; each part must be `LogStreamable`.
- **Emitting while holding your own subsystem lock is allowed**, because logging is the
  bottom of the lock hierarchy (§2). Emitting while holding a lock the logging path could
  reach is not allowed; nothing does that today.
- **The emission guard is advisory** (§2). `sim_estab_log` checks `log_is_init()` and emits
  after that check, so a record started while the subsystem is being torn down may be
  dropped. That is the contract: `log_deinit` requires the caller to stop emission first,
  and never waits for records in flight.
- **Logging inside a tick must go through a buffered path.** Engine core (Tier 3) systems run
  inside the parallel phase (`design_engine_core.md` §4.1), where a lock per record inside
  the log core would serialize exactly the work the phase runs in parallel. So the host
  ABI's `log fn` writes into a **per-worker buffer**, which the engine drains at the phase
  boundary, in worker order, and emits normally (`design_modding.md` §5.2). This buffer is
  designed but not built. It is not a new cross-thread mechanism and needs no row in the
  register of `design_engine_core.md` §1.1: the phase close is already a barrier that merges
  per-task staged buffers in fixed task order (§4.1 there), and the log buffer is one more
  buffer merged there. That fixed task order is also what gives the records a deterministic
  order. Records are not engine core state, so determinism would hold either way, but only the
  buffered form keeps the throughput. Engine code inside the phase uses the same path. What
  happens when a buffer fills is open
  ([Q38](open_question.md#q38-what-happens-when-a-per-worker-log-buffer-fills-during-a-phase)).
- Consumer-facing macros (`SIM_ESTAB_LOG_*`) live in `macro.h`.
- Unknown types are not implicitly stringified. Opt in per type via
  `SIM_ESTAB_LOG_ENABLE_OSTREAM(Type)` / `stream_via_ostream`.

## 8. Python binding conventions (`bind.cpp`)

- Binding is decided per API. Nothing is bound only because it exists, and a
  subsystem without bindings is complete: `gpu` and `viz` have none. The rules
  below say how to bind what is chosen.
- The nanobind module is `sim_estab._if`. Its submodules mirror the C++
  modules, so a binding is found from its C++ name:
  - a partition `<p>` of `sim_estab` is the submodule `_if.<p>`:
    `sim_estab::core::log` is `_if.log`, made with `def_submodule("log", ...)`;
  - a separate module `sim_estab.<m>` is the submodule `_if.<m>`;
  - a bound entity keeps its C++ name inside its submodule.

  The two kinds of submodule cannot collide, because a separate module's short
  name differs from every partition name (§1).
- Bind enums value-by-value.
- A bound exception keeps its C++ name: `x_error` is bound as `x_error`, and
  the root as `sim_estab_error`.
- Bind the root first, in `_if.error`, with `PyExc_RuntimeError` as its base.
  Bind each subsystem exception in its submodule with its C++ base as the
  Python base: `static nb::exception<x_error>(mod, "x_error", base)`. Python
  then sees the same tree as C++ (§6).
- Bind a base before the classes derived from it. nanobind tries the newest
  translator first, so a derived exception registered before its base would be
  raised as the base.
- Expose the init/deinit triad 1:1 (`log_init`, `log_deinit`, `log_is_init`,
  `enable_console`, `disable_console`).
- Template/variadic APIs get a concrete lambda wrapper, e.g.
  `log(channel, level, message)`. Give it `nb::arg` names and a docstring.
- Top-level `init` / `deinit` / `run` are placeholders. The engine lifecycle (the `Engine`
  object, pump primitives and a main loop in Python) is specified in `design_python_api.md`.

## 9. Checklist for a new subsystem

1. Interface in `module/sim_estab--<name>.cppm` as partition
   `sim_estab:<name>`. Use a separate module `sim_estab.<name>` if it drags
   heavy deps. Re-export from `sim_estab.cppm`. Put internals in a
   non-exported partition, which is not re-exported. Implementation unit in
   `src/<name>.cpp`. File names and folders follow §1 "File layout".
2. Export `<name>_error`, derived from `sim_estab_error` (§6).
3. Process-global state? → init/deinit/is_init triad (§2). Single `detail::`
   mutex. Idempotent both ways. Prefer a lock-free `is_init`.
4. Shared unique resource? → acquire/release refcount pair (§3). Throwing
   acquire, noexcept release, `_get`/`_is_init` queries. Also write
   down the constraints the resource itself imposes: per-process uniqueness,
   thread affinity, immutable creation options. Encode them the way the SDL
   example in §3 does.
5. Per-instance handle? → RAII context class (§4). Explicit throwing ctor,
   noexcept dtor, deleted copy/move. SBO pimpl (§5a) with `check_impl()`.
   `Impl::cleanup()` releases in reverse order. Ctor try/catch destroys the
   partial Impl and rethrows.
6. Third-party types leaking into the interface? Wrap them as opaque
   wrappers (§5b) or `void *` handles (§1).
7. Log lifecycle transitions on channel `sim_estab.<name>` per §7.
8. Decide what Python needs, and bind only that, per §8. Add tests:
   Boost.Test under `test/libsim_estab/`, and pytest under `test/` for what is
   bound.
