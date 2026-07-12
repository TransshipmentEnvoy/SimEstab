/**
 * @file sim_estab--util.cppm
 * @brief Module partition for general-purpose utilities
 *
 * Small self-contained helpers shared across subsystems. Each utility lives
 * in its own sub-namespace under sim_estab::core::util.
 */

export module sim_estab:util;

namespace sim_estab::core::util::thread_util {

/**
 * Check whether the calling thread is the process main thread.
 *
 * Platform semantics:
 * - Apple: pthread_main_np(), authoritative.
 * - Linux: gettid() == getpid() (raw syscall, works on any glibc/musl),
 *   authoritative.
 * - Windows: compares against the thread id captured when this library was
 *   loaded. Best-effort heuristic - wrong if the DLL is loaded from a
 *   non-main thread.
 *
 * @return true if called from the process main thread
 */
export [[nodiscard]] bool is_main_thread() noexcept;

} // namespace sim_estab::core::util::thread_util
