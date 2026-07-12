# Design Patterns — Lifecycle, Construction, Init/Deinit

Reference for the recurring patterns in the current codebase (`log`, `gpu`, `viz`
subsystems). New subsystems should follow these conventions unless there is a
documented reason not to.

Sources of truth as of writing:

- `src/libsim_estab/module/sim_estab--log.cppm` + `src/libsim_estab/src/log.cpp`
- `src/libsim_estab/module/sim_estab--gpu.cppm` + `src/libsim_estab/src/gpu.cpp`
- `src/libsim_estab/module/sim_estab.viz/viz.cppm` + `src/libsim_estab/src/viz/viz.cpp`
- `src/sim_estab_ext/bind.cpp` (Python bindings)

---

## 1. Module organization

- One primary named module `sim_estab`; subsystems are **module partitions**
  (`sim_estab:log`, `sim_estab:gpu`, `sim_estab:util`) re-exported by
  `sim_estab.cppm` (`export import :log;`). Optional/heavy subsystems that
  pull extra dependencies live in a **separate module** (`sim_estab.viz`)
  which `import sim_estab;`s the core. Small cross-cutting helpers go in the
  `:util` partition as sub-namespaces of `sim_estab::core::util` (e.g.
  `util::thread_util::is_main_thread()`).
- Interface = `module/*.cppm` (declarations + doc comments + small inline/
  template code). Implementation = `src/*.cpp` module implementation units
  (`module sim_estab;` without `export`), which include the heavy third-party
  headers (Boost.Log, SDL3) in their **global module fragment** only.
- Modules cannot export macros, so macros stay in ordinary headers under
  `include/` (`macro.h` for `SIM_ESTAB_LOG_*`, `compat.h` for std headers the
  interface needs — hybrid approach because g++ header export support is
  incomplete). Consumers do `import sim_estab;` + `#include <sim_estab/macro.h>`.
- Foreign struct types (e.g. `SDL_GPUDevice`) must **not** be forward-declared
  in a module interface: module type mangling makes them incompatible with the
  header-defined type. Expose them as opaque `void *` aliases instead
  (`SDL_GPUDevice_ptr`, `ComputeBufferHandle`) and `static_cast` in the
  implementation unit.

## 2. Subsystem lifecycle: `X_init` / `X_deinit` / `X_is_init` triad

Used for process-global subsystems that are initialized once, not per-object
(pattern: logging).

```cpp
export void X_init();                 // may throw; idempotent (second call is a no-op)
export void X_deinit() noexcept;      // idempotent, exception-safe, allows re-init afterwards
export bool X_is_init() noexcept;     // pure query
```

Implementation rules:

- All mutable state lives in a `namespace detail { ... }` inside the
  implementation unit: one `static std::mutex`, a `static bool init_status`,
  plus tracking containers (e.g. `flat_map<sink_type, sink_ptr> sink_map`).
- **Single global mutex per subsystem**, taken as the first line of every
  public function (including the `_is_init` query) — one lock, consistent
  order, no deadlock surface.
- `init` starts by checking `init_status` and returning early; it then does a
  defensive cleanup (flush, clear tracking map, remove stale sinks) before
  building fresh state, and sets `init_status = true` last.
- `deinit` flushes pending work first, then tears down in reverse order,
  resets `init_status = false` so `init` can be called again. It is `noexcept`
  and safe to call when never initialized.
- Optional feature toggles follow the same shape: `enable_Y()` /
  `disable_Y() noexcept`, both idempotent, both no-ops if the subsystem is not
  initialized (they check `init_status` and the tracking map).
- Hot-path helpers guard themselves: `sim_estab_log()` returns silently when
  `!log_is_init()` instead of throwing.

## 3. Ref-counted shared globals: `acquire` / `release` pair

Used when several independent objects must share one process-wide resource
(pattern: SDL context, shared GPU device — SDL3 GPU does not support multiple
devices per process).

```cpp
export void SDL_ctx_acquire(SDL_InitFlags subsystems = 0);   // throws on failure
export void SDL_ctx_release() noexcept;                      // no-op if never acquired
export [[nodiscard]] bool SDL_ctx_is_initialized() noexcept;

export [[nodiscard]] SDL_GPUDevice_ptr GPU_device_acquire(bool debug = false, bool low_power = false);
export void GPU_device_release() noexcept;
export [[nodiscard]] SDL_GPUDevice_ptr GPU_device_get() noexcept; // nullptr if not initialized
export [[nodiscard]] bool GPU_device_is_initialized() noexcept;
```

Implementation rules:

- `detail` namespace holds `static std::mutex`, `static int ref_count`, the
  resource pointer/flags. Every function locks the mutex first.
- **First acquire creates, last release destroys.** `release` decrements and
  only tears down at zero; calling release with `ref_count <= 0` is a safe
  no-op.
- `acquire` may throw (`gpu_error`); on the failure path it restores all
  `detail` state to the "never created" values before throwing (see
  `GPU_device_acquire` resetting `debug_mode`/`prefer_low_power`).
- Capability parameters are **first-caller-wins**: additional SDL subsystems
  requested on later acquires are initialized additively and accumulated
  (never shut down until refcount zero); creation options that cannot change
  (GPU debug mode, power preference) log a `warning` and are ignored if a
  later caller asks for different values.
- Layering/preconditions are enforced with a thrown error, not implicit
  auto-init: `GPU_device_acquire` throws if `SDL_ctx_is_initialized()` is
  false — the *caller* (context object) is responsible for acquiring
  dependencies in order.
- Every ref-count change is logged at `debug` with the new count; create/
  destroy at `info`.
- **First acquisition must happen on the process main thread** (enforced:
  `SDL_ctx_acquire` throws `gpu_error` when `ref_count == 0` off the main
  thread, checked via `core::util::thread_util::is_main_thread()` from the
  `sim_estab:util` partition). SDL defines
  its video/main thread as whichever thread first initializes video, and on
  Apple platforms that must be the real main thread. Worker threads may bump
  and release references freely once a reference exists. Recommended usage:
  acquire an app-lifetime reference on the main thread at startup so the
  refcount never returns to zero mid-run (otherwise the next first-acquire
  re-runs the main-thread requirement on whatever thread gets there first).

## 4. RAII context objects (`ComputeContext`, `VizContext`)

Per-instance handles whose constructor acquires everything and whose destructor
releases everything. This is the public face over the acquire/release layer.

Interface rules:

- Constructor `explicit`, takes user-facing options with defaults, `throws`
  subsystem error on any failure. Destructor is `noexcept`.
- **Non-copyable and non-movable** (all four special members `= delete`) —
  SDL resources cannot be safely moved and the SBO pimpl pins the address.
- Queries are `const`, `[[nodiscard]]` where the result is the whole point
  (`has_device()`, `get_device_info()`).

Construction order (and the exact reverse on cleanup):

```
ctor: placement-new Impl  →  SDL_ctx_acquire(subsystems)  →  GPU_device_acquire(...)
      →  create owned resources (window, claim window for device)  →  log backend info
dtor: Impl::~Impl → cleanup(): release window claim → destroy window
      → GPU_device_release() → SDL_ctx_release()
```

Implementation rules:

- All state lives in `struct Impl` (defined only in the .cpp), itself
  non-copyable/non-movable, with `~Impl() { cleanup(); }` and a
  `void cleanup() noexcept` that releases **in reverse order of creation** and
  nulls/clears state as it goes. Shared resources are marked "not owned" in
  comments and released via the ref-count API, never destroyed directly.
- **cleanup() releases only what was provably acquired.** Every acquisition is
  witnessed by Impl state — a non-null pointer (`gpu_device`, `window`) or a
  boolean flag set immediately after the acquiring call succeeds
  (`sdl_ctx_acquired`, `window_claimed`) — and each release in `cleanup()` is
  guarded by that witness. Never release a ref-counted global unconditionally:
  if the acquire threw, an unconditional release would decrement a reference
  held by another live context.
- **Constructor exception safety**: if any step throws, destroy the
  partially-built Impl and null the cached pointer before rethrowing, so the
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

  Wrap the **whole constructor body** (everything after the placement-new,
  including trailing logging) in one `try { ... } catch (...)` — `catch (...)`,
  not a specific exception type, so `std::bad_alloc` and friends also trigger
  rollback. Boolean flags in Impl (`sdl_ctx_acquired`, `window_claimed`)
  record sub-steps that need distinct undo actions.
- Cross-subsystem errors are **translated at the boundary**: `VizContext`
  catches `gpu_error` and rethrows as `viz_error` with the original message
  prepended with context (`"SDL initialization failed: " + e.what()`).
- Debug/release divergence is decided at compile time in the ctor
  (`#ifdef NDEBUG` → GPU validation layers on in debug builds only).

## 5. Fast pimpl (SBO) — two variants

No heap allocation for pimpl; the implementation object is placement-new'd
into an aligned in-object buffer.

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

- `static_assert(sizeof(Impl) <= ImplSize)` / `alignof` checks go **inside the
  constructor** (the only place where Impl is complete).
- The cached `impl_` pointer avoids repeated `std::launder`; it doubles as the
  "constructed" flag: destructor does `if (impl_) { impl_->~Impl(); impl_ = nullptr; }`.
- **Mutating operations** call `check_impl()` first — on null it logs
  `critical` ("programming error") and `std::abort()`s. **Read-only queries**
  instead degrade gracefully: `if (!impl_) return {};` (see the `VizContext`
  getters, `has_device()`).

### 5b. Opaque wrapper style (`record`, `record_ostream`, `logger`, `logger_mt`)

For 1:1 wrappers that completely hide a third-party type (Boost.Log) from the
module interface:

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

- The .cpp has file-scope
  `static_assert(sizeof(wrapped) <= wrapper::storage_size, ...)` (and align).
- Every member is a thin forwarder: `static_cast<Wrapped*>(get_impl())`, call
  through. Constructors placement-new; destructor
  `std::launder(reinterpret_cast<Wrapped*>(storage_))` + `std::destroy_at`.
- **Copy/move semantics mirror the wrapped type exactly**: `record` is
  move-only, `record_ostream` is neither copyable nor movable, `logger[_mt]`
  is fully copyable/movable. Copy assignment uses copy-and-swap; each
  copyable/swappable wrapper also gets a free `swap()` overload.
- Friendship (`friend class logger;` etc.) grants the collaborating wrappers
  access to `get_impl()` instead of exposing it.

## 6. Error handling conventions

- One exception class per subsystem, exported from the module, deriving
  `std::runtime_error`, with the single canonical constructor:

  ```cpp
  export class x_error : public std::runtime_error {
  public:
      explicit x_error(const std::string& what) : std::runtime_error(what) {}
  };
  ```

- Direction of throw/noexcept: anything that **creates/acquires** may throw;
  anything that **releases/destroys/queries** is `noexcept` and tolerant of
  the never-initialized state.
- On a C-API failure: capture `SDL_GetError()` immediately, log at `error`,
  throw with `"<what failed>: " + error`. Clean up any locals acquired within
  the function (transfer buffers, command buffers, properties) on every early
  exit path before throwing — including `SDL_CancelGPUCommandBuffer` for
  unsubmitted command buffers.
- Argument validation up front, with specific messages
  (`"Cannot create buffer with size 0"`, `"Cannot dispatch with null pipeline"`);
  benign no-ops (empty upload span, zero-size download) return early instead
  of throwing.
- Use-after-destruction / uninitialized access is a **programming error**:
  `check_impl()` → log `critical` → `std::abort()`, never an exception.

## 7. Logging conventions

- Channels are hierarchical: `"sim_estab.<subsystem>"` (`sim_estab.gpu`,
  `sim_estab.viz`); components may extend (`module.subsystem.component`).
- Severity usage: lifecycle create/destroy at `info`; ref-count ticks,
  successful sub-steps and per-op traces at `debug`; recoverable/ignored
  mismatches at `warning`; failures that throw at `error`; abort-level
  programming errors at `critical`.
- Always log the failure **before** throwing.
- In-library code calls `sim_estab_log(channel, severity_level::x, parts...)`
  directly (variadic, each part `LogStreamable`); consumer-facing macros
  (`SIM_ESTAB_LOG_*`) live in `macro.h`. Unknown types are not implicitly
  stringified — opt in per type via `SIM_ESTAB_LOG_ENABLE_OSTREAM(Type)` /
  `stream_via_ostream`.

## 8. Python binding conventions (`bind.cpp`)

- nanobind module is `sim_estab._if`; each subsystem gets a
  `def_submodule("log", ...)`.
- Enums bound value-by-value; subsystem exceptions bound with
  `static nb::exception<x_error>(mod, "x_error", PyExc_RuntimeError)`.
- The init/deinit triad is exposed 1:1 (`log_init`, `log_deinit`,
  `log_is_init`, `enable_console`, `disable_console`); template/variadic APIs
  get a concrete lambda wrapper (`log(channel, level, message)`) with
  `nb::arg` names and a docstring.
- Top-level `init` / `deinit` / `run` exist as placeholders for the future
  engine lifecycle — keep that naming when the engine core lands.

## 9. Checklist for a new subsystem

1. Interface in `module/sim_estab--<name>.cppm` as partition `sim_estab:<name>`
   (or separate module `sim_estab.<name>` if it drags heavy deps); re-export
   from `sim_estab.cppm`; implementation unit in `src/<name>.cpp`.
2. Export `<name>_error : std::runtime_error`.
3. Process-global state? → init/deinit/is_init triad (§2) with a single
   `detail::` mutex, idempotent both ways.
4. Shared unique resource? → acquire/release refcount pair (§3), throwing
   acquire, noexcept release, `_get`/`_is_initialized` queries.
5. Per-instance handle? → RAII context class (§4): explicit throwing ctor,
   noexcept dtor, deleted copy/move, SBO pimpl (§5a) with `check_impl()`,
   Impl::cleanup() releasing in reverse order, ctor try/catch that destroys
   the partial Impl and rethrows.
6. Wrap third-party types leaking into the interface as opaque wrappers (§5b)
   or `void *` handles (§1).
7. Log lifecycle transitions on channel `sim_estab.<name>` per §7.
8. Bind to Python per §8; add tests (Boost.Test under `test/libsim_estab/`,
   pytest under `test/`).
