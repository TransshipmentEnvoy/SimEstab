/**
 * @file test_thread_util.cpp
 * @brief Tests for thread_util main-thread detection and SDL first-init enforcement
 *
 * These tests are headless-safe: the off-main-thread cases throw before any
 * SDL state is touched, and the main-thread SDL acquisition uses no subsystems.
 */

#include <boost/test/unit_test.hpp>

#include <thread>

// compat headers
#include <compat.h>

// import modules
import sim_estab;

namespace utf = boost::unit_test;
using namespace sim_estab::core::gpu;
namespace thread_util = sim_estab::core::util::thread_util;

//==============================================================================
// Test Suite
//==============================================================================

BOOST_AUTO_TEST_SUITE(thread_util_tests)

/**
 * @brief Main-thread detection returns true on the test main thread
 */
BOOST_AUTO_TEST_CASE(test_is_main_thread_on_main,
                     *utf::description("is_main_thread() is true on the process main thread")) {
    BOOST_TEST(thread_util::is_main_thread());
}

/**
 * @brief Main-thread detection returns false on a worker thread
 */
BOOST_AUTO_TEST_CASE(test_is_main_thread_on_worker,
                     *utf::description("is_main_thread() is false on a spawned thread")) {
    bool worker_is_main = true;
    std::thread worker([&worker_is_main] { worker_is_main = thread_util::is_main_thread(); });
    worker.join();

    BOOST_TEST(!worker_is_main);
}

/**
 * @brief First SDL acquisition from a worker thread is rejected
 */
BOOST_AUTO_TEST_CASE(test_first_sdl_acquire_off_main_throws,
                     *utf::description("SDL_ctx_acquire with refcount 0 throws gpu_error off the main thread")) {
    BOOST_REQUIRE(!SDL_ctx_is_initialized());

    bool threw_gpu_error = false;
    std::thread worker([&threw_gpu_error] {
        try {
            SDL_ctx_acquire(0);
        } catch (const gpu_error&) {
            threw_gpu_error = true;
        }
    });
    worker.join();

    BOOST_TEST(threw_gpu_error);
    BOOST_TEST(!SDL_ctx_is_initialized());
}

/**
 * @brief ComputeContext construction from a worker thread is rejected before touching SDL
 */
BOOST_AUTO_TEST_CASE(test_compute_context_off_main_throws,
                     *utf::description("ComputeContext ctor on a worker thread throws gpu_error with refcount 0")) {
    BOOST_REQUIRE(!SDL_ctx_is_initialized());

    bool threw_gpu_error = false;
    std::thread worker([&threw_gpu_error] {
        try {
            ComputeContext ctx;
        } catch (const gpu_error&) {
            threw_gpu_error = true;
        }
    });
    worker.join();

    BOOST_TEST(threw_gpu_error);
    BOOST_TEST(!SDL_ctx_is_initialized());
}

/**
 * @brief Worker threads may acquire additional references once main holds one
 */
BOOST_AUTO_TEST_CASE(test_worker_reacquire_allowed,
                     *utf::description("SDL_ctx_acquire from a worker succeeds while main holds a reference")) {
    BOOST_REQUIRE(!SDL_ctx_is_initialized());

    // First acquisition on the main thread, no subsystems (headless-safe)
    SDL_ctx_acquire(0);
    BOOST_TEST(SDL_ctx_is_initialized());

    bool worker_acquired = false;
    std::thread worker([&worker_acquired] {
        try {
            SDL_ctx_acquire(0);
            worker_acquired = true;
            SDL_ctx_release();
        } catch (const gpu_error&) {
        }
    });
    worker.join();

    BOOST_TEST(worker_acquired);
    BOOST_TEST(SDL_ctx_is_initialized());

    SDL_ctx_release();
    BOOST_TEST(!SDL_ctx_is_initialized());
}

BOOST_AUTO_TEST_SUITE_END()
