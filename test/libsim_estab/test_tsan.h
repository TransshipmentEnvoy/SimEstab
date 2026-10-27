/**
 * @file test_tsan.h
 * @brief ThreadSanitizer setup shared by the test mains
 *
 * test_main.cpp and test_main_viz.cpp include this header before they import the module,
 * and call its function in their global fixture.
 */

#ifndef LIBSIM_ESTAB__TEST__TEST_TSAN_H
#define LIBSIM_ESTAB__TEST__TEST_TSAN_H

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <filesystem>

/**
 * @brief Under ThreadSanitizer, let the Vulkan loader see only lavapipe
 *
 * The NVIDIA driver starts threads through a pthread_create it looks up with dlsym, so TSan
 * never sees them and their first allocation crashes TSan. A VK_ICD_FILENAMES or
 * VK_DRIVER_FILES already set wins. The manifest name differs between distributions.
 * Outside a TSan build on Linux this does nothing.
 */
inline void set_tsan_vulkan_driver() {
#if defined(__linux__) && defined(__SANITIZE_THREAD__)
    if (std::getenv("VK_ICD_FILENAMES") != nullptr || std::getenv("VK_DRIVER_FILES") != nullptr) {
        return;
    }
    for (const char *icd : {"/usr/share/vulkan/icd.d/lvp_icd.json", "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"}) {
        if (std::filesystem::exists(icd)) {
            ::setenv("VK_ICD_FILENAMES", icd, 0);
            return;
        }
    }
    BOOST_TEST_MESSAGE("No lavapipe manifest found; the Vulkan loader sees every driver under TSan");
#endif
}

#endif // LIBSIM_ESTAB__TEST__TEST_TSAN_H
