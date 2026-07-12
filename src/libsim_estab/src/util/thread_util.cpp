module;

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#error "thread_util: is_main_thread() not implemented for this platform"
#endif

module sim_estab;

namespace sim_estab::core::util::thread_util {

#if defined(_WIN32)
namespace detail {
// Windows has no API to identify the process main thread. Capture the id of
// the thread that loads this library (static initialization runs on the
// loader's thread, which is the main thread in normal use).
static const DWORD main_thread_id = ::GetCurrentThreadId();
} // namespace detail
#endif

bool is_main_thread() noexcept {
#if defined(__APPLE__)
    return ::pthread_main_np() != 0;
#elif defined(__linux__)
    // The kernel guarantees the main thread's TID equals the PID. Raw syscall
    // instead of gettid() so this builds on glibc < 2.30 and musl.
    return ::syscall(SYS_gettid) == static_cast<long>(::getpid());
#elif defined(_WIN32)
    return ::GetCurrentThreadId() == detail::main_thread_id;
#endif
}

} // namespace sim_estab::core::util::thread_util
