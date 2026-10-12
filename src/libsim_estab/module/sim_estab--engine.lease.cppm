/**
 * @file sim_estab--engine.lease.cppm
 * @brief Non-exported module partition for the lease
 *
 * A lease counts the running holders of something a closer wants to shut: the running
 * submits of an endpoint (design_engine_core.md §5.1), or the running native calls of the
 * engine (design_python_api.md §6). The two are one protocol, and this is their one
 * building block. A holder enters, does its work and leaves. The closer closes admission
 * with a read-modify-write on the state, then waits for the holders still running, under
 * a deadline.
 *
 * The mechanism register row (design_engine_core.md §1.1):
 *
 * - Atomic objects: `state` and `active`, 4 bytes each, always lock-free on x86-64.
 * - Memory orders: a holder loads `state` with acquire, adds to `active` with acq_rel, and
 *   loads `state` again with acquire; it subtracts with acq_rel. The closer leaves OPEN
 *   with an acq_rel read-modify-write, an exchange or a compare-exchange, and reads
 *   `active` only with a read-modify-write, so it cannot miss a holder that saw OPEN.
 * - Linearization point: a lease, at the second load of `state`; closing, at the
 *   read-modify-write that leaves OPEN.
 * - Progress: the holder side is wait-free. The closer parks on `m` until the last running
 *   holder leaves, bounded by its deadline.
 * - Failure: a deadline that passes leaves the state as the closer set it. The closer takes
 *   the failed path, and reclaims nothing.
 *
 * The closer's park is the one stated exception to the wake-up rule of
 * design_engine_core.md §3.3: a holder changes the count outside `m`. Two things keep it
 * correct. The last holder to leave a lease that is not OPEN takes `m` before it notifies,
 * so the notify cannot fall between the closer's check and its sleep. And the closer reads
 * the count with a read-modify-write, so that holder cannot miss that the lease is closing.
 * x86 and ThreadSanitizer cannot show the second one failing; the litmus tests in
 * test/litmus/ check it with herd7.
 *
 * This partition is internal: an importer of sim_estab cannot see it. Its tests are units
 * of the module (design_patterns.md §10).
 */

// Global module fragment - minimal headers only
module;

// Standard library headers
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Module declaration
module sim_estab:engine.lease;

/**
 * @namespace sim_estab::core::engine::lease
 * @brief The lease: the building block of the endpoint lease and the operation lease
 */
namespace sim_estab::core::engine::lease {

/// The lease's state. The endpoint lease goes OPEN, CLOSING, CLOSED, and may open again;
/// the operation lease goes OPEN, CLOSING, CLOSED, or FAILED when its close wait times out.
enum class LeaseState : std::uint32_t { OPEN, CLOSING, CLOSED, FAILED };

/**
 * The lease (design_engine_core.md §5.1, design_python_api.md §6)
 *
 * Two roles use it. A holder, on any thread, enters and leaves around its work. The closer
 * closes admission and waits for the running holders. Whoever owns it keeps it alive until
 * the last holder has left and the closer is done.
 */
struct Lease {
    using Clock = std::chrono::steady_clock;

    std::atomic<LeaseState> state;     ///< admission: a holder enters while OPEN
    std::atomic<std::uint32_t> active; ///< running holders
    std::mutex m;                      ///< the closer's park; held for loads, stores and notifies only
    std::condition_variable cv;        ///< the closer waits here; the last holder out of a closing lease notifies

    explicit Lease(LeaseState initial) noexcept : state(initial), active(0) {}

    Lease(const Lease&)            = delete;
    Lease& operator=(const Lease&) = delete;

    /// Holder: enter. false: the lease is not OPEN, and nothing was counted.
    [[nodiscard]] bool try_enter() noexcept {
        if (state.load(std::memory_order_acquire) != LeaseState::OPEN) {
            return false;
        }
        active.fetch_add(1, std::memory_order_acq_rel);
        if (state.load(std::memory_order_acquire) != LeaseState::OPEN) {
            leave();
            return false;
        }
        return true; // LINEARIZATION POINT: the second load saw OPEN
    }

    /// Holder: leave. The last to leave a lease that is not OPEN wakes the closer.
    void leave() noexcept {
        if (active.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
            state.load(std::memory_order_acquire) != LeaseState::OPEN) {
            // The closer checks the count under m, so this notify cannot fall between its
            // check and its sleep.
            std::lock_guard lk(m);
            cv.notify_all();
        }
    }

    /**
     * Closer, once the state is no longer OPEN: wait for every running holder to leave, at
     * most until `until`
     *
     * @return true when no holder is running. false if `until` passed first: the state is
     *         as the closer left it, and the closer takes the failed path.
     */
    [[nodiscard]] bool wait_for_holders(Clock::time_point until) {
        // A read-modify-write, not a load. It reads the latest count, so a holder whose lease
        // saw OPEN is counted here, or has itself seen the closing state
        // (design_engine_core.md §5.1). test/litmus/ checks this with herd7; x86 and
        // ThreadSanitizer cannot.
        std::unique_lock lk(m);
        return cv.wait_until(lk, until, [&] { return active.fetch_add(0, std::memory_order_acq_rel) == 0; });
    }
};

/// Leaves the lease on every way out of a holder that entered it
class LeaseGuard {
public:
    explicit LeaseGuard(Lease& held) noexcept : lease_(held) {}
    LeaseGuard(const LeaseGuard&)            = delete;
    LeaseGuard& operator=(const LeaseGuard&) = delete;
    ~LeaseGuard() { lease_.leave(); }

private:
    Lease& lease_;
};

} // namespace sim_estab::core::engine::lease
