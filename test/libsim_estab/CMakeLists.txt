cmake_minimum_required(VERSION 3.28)

# Enable CTest support
enable_testing()

# Test build options
option(TEST_ENABLE_LOGGING "Enable logging in test output" ON)

# Add test directory to include path
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Test dependencies
find_package(Boost REQUIRED unit_test_framework)
message(STATUS "Test: Using Boost.Test from Boost installation")

#==============================================================================
# Main test executable (non-visualization tests)
#==============================================================================
add_executable(libsim_estab_test
    test_main.cpp
    test_log.cpp
    test_compute.cpp
    test_thread_util.cpp
)

# Enable C++20 modules if supported by compiler
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(libsim_estab_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fmodules-ts>)
endif()

# Link with the main library
target_link_libraries(libsim_estab_test PRIVATE
    sim_estab
    Boost::log
    Boost::unit_test_framework
)

# Set C++ standard for tests
target_set_cxx_standard(libsim_estab_test)

# sanitizers
if(LIBSIM_ESTAB__SANITIZE_ADDRESS)
    target_enable_asan(libsim_estab_test)
endif()

if(LIBSIM_ESTAB__SANITIZE_UNDEFINED)
    target_enable_ubsan(libsim_estab_test)
endif()

if(LIBSIM_ESTAB__SANITIZE_THREAD)
    target_enable_tsan(libsim_estab_test)
endif()

# Add test to CTest
add_test(NAME libsim_estab_test COMMAND libsim_estab_test)

# Set test properties
set_tests_properties(libsim_estab_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp;TSAN_OPTIONS=ignore_noninstrumented_modules=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp"
)

message(STATUS "Test: Built libsim_estab_test executable")

#==============================================================================
# Visualization test executable (requires display environment)
#==============================================================================
add_executable(libsim_estab_viz_test
    test_main_viz.cpp
    test_viz.cpp
)

# Enable C++20 modules if supported by compiler
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(libsim_estab_viz_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fmodules-ts>)
endif()

# Link with the main library and visualization module
target_link_libraries(libsim_estab_viz_test PRIVATE
    sim_estab
    Boost::log
    Boost::unit_test_framework
)

# Set C++ standard for tests
target_set_cxx_standard(libsim_estab_viz_test)

# sanitizers
if(LIBSIM_ESTAB__SANITIZE_ADDRESS)
    target_enable_asan(libsim_estab_viz_test)
endif()

if(LIBSIM_ESTAB__SANITIZE_UNDEFINED)
    target_enable_ubsan(libsim_estab_viz_test)
endif()

if(LIBSIM_ESTAB__SANITIZE_THREAD)
    target_enable_tsan(libsim_estab_viz_test)
endif()

# Add test to CTest (disabled by default since it requires display)
add_test(NAME libsim_estab_viz_test COMMAND libsim_estab_viz_test)

# Set test properties - disabled by default for CI environments without display
set_tests_properties(libsim_estab_viz_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    LABELS "visualization;requires_display"
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp;TSAN_OPTIONS=ignore_noninstrumented_modules=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp"
)

message(STATUS "Test: Built libsim_estab_viz_test executable (requires display)")

# End of test
message(STATUS "Test: Use 'ctest' to run tests")