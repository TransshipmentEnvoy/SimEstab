cmake_minimum_required(VERSION 3.28)

# Enable CTest support
enable_testing()

# Test build options
option(TEST_ENABLE_LOGGING "Enable logging in test output" ON)

# Add test directory to include path
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Test dependencies
find_package(Boost REQUIRED unit_test_framework)
message(STATUS "Tests: Using Boost.Test from Boost installation")

# Test executable
add_executable(libsim_estab_tests
    test_main.cpp
    test_log.cpp
)

# Enable C++20 modules if supported by compiler
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(libsim_estab_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fmodules-ts>)
endif()

# Link with the main library
target_link_libraries(libsim_estab_tests PRIVATE
    sim_estab
    Boost::log
    Boost::unit_test_framework
)

# Set C++ standard for tests
set_target_properties(libsim_estab_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)

# Add test to CTest
add_test(NAME libsim_estab_tests COMMAND libsim_estab_tests)

# Set test properties
set_tests_properties(libsim_estab_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

message(STATUS "Tests: Built libsim_estab_tests executable")
message(STATUS "Tests: Use 'ctest' to run tests")