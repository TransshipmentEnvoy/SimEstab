# Design Patterns — Lifecycle, Construction, Init/Deinit

Reference for the recurring patterns in the current codebase (`log`, `gpu`,
`viz` subsystems). New subsystems should follow these conventions. A deviation
needs a documented reason.

Sources of truth as of writing:

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
- Macros stay in ordinary headers. Foreign types cross the interface as
  opaque `void *` aliases.

**Rules**

- One primary named module: `sim_estab`.
- Subsystems are **module partitions**: `sim_estab:log`, `sim_estab:gpu`,
  `sim_estab:util`.
- `sim_estab.cppm` re-exports them (`export import :log;`).
- Optional/heavy subsystems that pull extra dependencies live in a
  **separate module** (`sim_estab.viz`). That module does `import sim_estab;`.
- Small cross-cutting helpers go in the `:util` partition. They live in
  sub-namespaces of `sim_estab::core::util`. Example:
  `util::thread_util::is_main_thread()`.
- Interface = `module/*.cppm`. It holds declarations, doc comments, and small
  inline/template code.
- Implementation = `src/*.cpp` module implementation units
  (`module sim_estab;` without `export`).
- Implementation units include the heavy third-party headers (Boost.Log,
  SDL3). They do so in their **global module fragment** only.
- Modules cannot export macros. So macros stay in ordinary headers under
  `include/`:
  - `macro.h` for `SIM_ESTAB_LOG_*`.
  - `compat.h` for std headers the interface needs. This hybrid exists
    because g++ header export support is incomplete.
- Consumers do `import sim_estab;` + `#include <sim_estab/macro.h>`.
- Never forward-declare foreign struct types (e.g. `SDL_GPUDevice`) in a
  module interface. Module type mangling makes them incompatible with the
  header-defined type.
- Instead, expose them as opaque `void *` aliases (`SDL_GPUDevice_ptr`,
  `ComputeBufferHandle`). `static_cast` in the implementation unit.

## 2. Subsystem lifecycle: `X_init` / `X_deinit` / `X_is_init` triad

**TL;DR**

- For process-global subsystems that initialize once.
- Three functions: throwing idempotent `init`, noexcept idempotent `deinit`,
  noexcept `is_init` query.
- One global mutex per subsystem guards everything.

### Pattern

Use for process-global subsystems. They initialize once, not per object.

```cpp
export void X_init();                 // may throw; idempotent (second call is a no-op)
export void X_deinit() noexcept;      // idempotent, exception-safe, allows re-init afterwards
export bool X_is_init() noexcept;     // pure query
```

Implementation rules:

- All mutable state lives in a `namespace detail { ... }` inside the
  implementation unit. That state is:
  - one `static std::mutex`;
  - a `static bool init_status`;
  - whatever tracking containers the subsystem needs.
- Use a **single global mutex per subsystem**. Take it as the first line of
  every public function, including the `_is_init` query. One lock, consistent
  order, no deadlock surface.
- `init` behavior, in order:
  1. Check `init_status`. Return early if already set.
  2. Do a defensive cleanup of any stale state.
  3. Build fresh state.
  4. Set `init_status = true` last.
- `deinit` behavior, in order:
  1. Flush pending work.
  2. Tear down in reverse order.
  3. Reset `init_status = false`, so `init` can run again.
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
- Feature toggles: `enable_console()` / `disable_console()` (likewise for the
  file sink). They check `init_status` and the tracking map.
- Hot-path guard: `sim_estab_log()` returns silently when `!log_is_init()`.

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
export [[nodiscard]] bool X_is_initialized() noexcept;
export [[nodiscard]] X_handle X_get() noexcept;  // if the resource is handle-shaped;
                                                 // nullptr if not initialized
```

Implementation rules:

- The `detail` namespace holds:
  - a `static std::mutex`;
  - a `static int ref_count`;
  - the resource pointer/flags.
- Every function locks the mutex first.
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
- Log every refcount change at `debug`, with the new count. Log
  create/destroy at `info`.

### Example: SDL context and shared GPU device (`sim_estab:gpu`)

```cpp
export void SDL_ctx_acquire(SDL_InitFlags subsystems = 0);   // throws on failure
export void SDL_ctx_release() noexcept;                      // no-op if never acquired
export [[nodiscard]] bool SDL_ctx_is_initialized() noexcept;

export [[nodiscard]] SDL_GPUDevice_ptr GPU_device_acquire(bool debug = false, bool low_power = false);
export void GPU_device_release() noexcept;
export [[nodiscard]] SDL_GPUDevice_ptr GPU_device_get() noexcept; // nullptr if not initialized
export [[nodiscard]] bool GPU_device_is_initialized() noexcept;
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
  `SDL_ctx_is_initialized()` is false.
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

- `[[nodiscard]]` queries: `has_device()`, `get_device_info()`.
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
    static constexpr size_t ImplSize  = 512;       // headroom, not exact
    static constexpr size_t ImplAlign = alignof(void *);

    alignas(ImplAlign) unsigned char impl_buffer_[ImplSize];
    Impl *impl_ = nullptr;                         // cached; nullptr = not constructed

    void check_impl() const noexcept;              // aborts on null, even in release
```

- Put `static_assert(sizeof(Impl) <= ImplSize)` (and the `alignof` check)
  **inside the constructor**. That is the only place where Impl is complete.
- The cached `impl_` pointer avoids repeated `std::launder`.
- `impl_` doubles as the "constructed" flag. The destructor does:
  `if (impl_) { impl_->~Impl(); impl_ = nullptr; }`.
- **Mutating operations** call `check_impl()` first. On null, it logs
  `critical` ("programming error") and calls `std::abort()`.
- **Read-only queries** degrade gracefully instead: `if (!impl_) return {};`.
  See the `VizContext` getters and `has_device()`.

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

- One exception class per subsystem.
- Create/acquire may throw. Release/destroy/query never throw.
- Log the failure before throwing.
- Programming errors abort. They do not throw.

Rules:

- One exception class per subsystem. Export it from the module. Derive from
  `std::runtime_error`. Use the single canonical constructor:

  ```cpp
  export class x_error : public std::runtime_error {
  public:
      explicit x_error(const std::string& what) : std::runtime_error(what) {}
  };
  ```

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
- Use-after-destruction / uninitialized access is a **programming error**:
  `check_impl()` → log `critical` → `std::abort()`. Never an exception.

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
- Consumer-facing macros (`SIM_ESTAB_LOG_*`) live in `macro.h`.
- Unknown types are not implicitly stringified. Opt in per type via
  `SIM_ESTAB_LOG_ENABLE_OSTREAM(Type)` / `stream_via_ostream`.

## 8. Python binding conventions (`bind.cpp`)

- The nanobind module is `sim_estab._if`. Each subsystem gets a
  `def_submodule("log", ...)`.
- Bind enums value-by-value.
- Bind subsystem exceptions with
  `static nb::exception<x_error>(mod, "x_error", PyExc_RuntimeError)`.
- Expose the init/deinit triad 1:1 (`log_init`, `log_deinit`, `log_is_init`,
  `enable_console`, `disable_console`).
- Template/variadic APIs get a concrete lambda wrapper, e.g.
  `log(channel, level, message)`. Give it `nb::arg` names and a docstring.
- Top-level `init` / `deinit` / `run` are placeholders for the future engine
  lifecycle. Keep that naming when the engine core lands.

## 9. Checklist for a new subsystem

1. Interface in `module/sim_estab--<name>.cppm` as partition
   `sim_estab:<name>`. Use a separate module `sim_estab.<name>` if it drags
   heavy deps. Re-export from `sim_estab.cppm`. Implementation unit in
   `src/<name>.cpp`.
2. Export `<name>_error : std::runtime_error`.
3. Process-global state? → init/deinit/is_init triad (§2). Single `detail::`
   mutex. Idempotent both ways.
4. Shared unique resource? → acquire/release refcount pair (§3). Throwing
   acquire, noexcept release, `_get`/`_is_initialized` queries. Also write
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
8. Bind to Python per §8. Add tests: Boost.Test under `test/libsim_estab/`,
   pytest under `test/`.
