cmake_minimum_required (VERSION 3.28)

# sanity check
if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "Building in-source is not supported! Create a build dir and remove ${CMAKE_SOURCE_DIR}/CMakeCache.txt")
endif()

# import early cmake module
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# project
project(libsim_estab)
set(PROJECT_VERSION "0.0.1")

# import cmake module
enable_language(CXX)

include(GNUInstallDirs)
include(CMakePrintHelpers)
include(CMakePackageConfigHelpers)

include(util/expr_util)
include(dep)
include(cpp_standard)
include(warning)
include(lto)
include(sanitizer)
include(shader_tools)

# option
option(BUILD_TESTS "Build C++ tests for libsim_estab" OFF)
option(LIBSIM_ESTAB__SANITIZE_ADDRESS "Enable address sanitizer for libsim_estab" OFF)
option(LIBSIM_ESTAB__SANITIZE_UNDEFINED "Enable undefined behavior sanitizer for libsim_estab" OFF)
option(LIBSIM_ESTAB__SANITIZE_THREAD "Enable thread sanitizer for libsim_estab" OFF)

# common path
cmake_path(GET CMAKE_CURRENT_LIST_FILE PARENT_PATH project_path_current)
cmake_path(GET project_path_current PARENT_PATH project_path_prefix)
cmake_path(GET project_path_prefix PARENT_PATH project_path_prefix)
message(STATUS "${PROJECT_NAME}: project path prefix: ${project_path_prefix}")
message(STATUS "${PROJECT_NAME}: project path: ${project_path_current}")

# dependency
find_package(Boost REQUIRED COMPONENTS headers filesystem nowide log)
message(STATUS "${PROJECT_NAME}: use boost: ${Boost_DIR}")

find_package(fmt REQUIRED)
message(STATUS "${PROJECT_NAME}: use fmt: ${fmt_DIR}")

find_package(rang REQUIRED)
message(STATUS "${PROJECT_NAME}: use rang: ${rang_DIR}")

find_package(range-v3 REQUIRED)
message(STATUS "${PROJECT_NAME}: use range-v3: ${range-v3_DIR}")

find_package(tsl-robin-map REQUIRED)
message(STATUS "${PROJECT_NAME}: use tsl-robin-map: ${tsl-robin-map_DIR}")

find_package(Eigen3 5 REQUIRED NO_MODULE)
message(STATUS "${PROJECT_NAME}: use Eigen3: ${Eigen3_DIR}")

# geometry
find_package(CGAL REQUIRED)
message(STATUS "${PROJECT_NAME}: use CGAL: ${CGAL_DIR}")

## dep: graphics
find_package(SDL3 3.4 REQUIRED CONFIG COMPONENTS GPU)
message(STATUS "${PROJECT_NAME}: using SDL3 from ${SDL3_DIR}")

## dep: vulkan
find_package(Vulkan REQUIRED)
message(STATUS "${PROJECT_NAME}: using Vulkan version ${Vulkan_VERSION}")

## dep: shader tools
find_package(glslang REQUIRED)
message(STATUS "${PROJECT_NAME}: use glslang: ${glslang_DIR}")
find_package(spirv-cross REQUIRED)
message(STATUS "${PROJECT_NAME}: use spirv-cross: ${spirv-cross_DIR}")
find_shader_tools()

## dep: config
find_package(nlohmann_json 3.12.0 REQUIRED)
message(STATUS "${PROJECT_NAME}: use nlohmann_json: ${nlohmann_json_DIR}")

## dep: bundled, to be built together
message(STATUS "${PROJECT_NAME}: use bundled project prefix: ${project_path_prefix}")

# compile & link property
check_lto_support(LTO_SUPPORTED)

# main library
add_library(sim_estab SHARED)
add_library(libsim_estab::sim_estab ALIAS sim_estab)
target_set_cxx_standard(sim_estab)
target_sources(sim_estab PRIVATE
    # module-based implementation (imports module, not a module unit itself)
    src/log.cpp
    src/gpu.cpp
    src/viz/viz.cpp
    src/util/thread_util.cpp
)
# C++20 module sources
target_sources(sim_estab PUBLIC
    FILE_SET CXX_MODULES FILES
    module/sim_estab.cppm
    module/sim_estab--log.cppm
    module/sim_estab--gpu.cppm
    module/sim_estab--util.cppm
    module/sim_estab.viz/viz.cppm)
target_link_libraries(sim_estab PUBLIC
    Boost::headers
    Boost::filesystem
    Boost::nowide
    Boost::log
    range-v3::range-v3
    fmt::fmt
    rang::rang
    tsl::robin_map
    Eigen3::Eigen
    CGAL::CGAL)
target_link_libraries(sim_estab PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan)
target_include_directories(sim_estab PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(sim_estab PROPERTIES
    INSTALL_RPATH "\$ORIGIN")
# TODO: change symbol visibility in the future
set_target_properties(sim_estab PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
# LTO - enable if supported (required for cross-module inlining)
# Only enable LTO for Release-related builds, not for Debug
if(LTO_SUPPORTED)
    target_enable_lto_release_only(sim_estab)
endif()
# sanitizers
if(LIBSIM_ESTAB__SANITIZE_ADDRESS)
    target_enable_asan(sim_estab)
endif()
if(LIBSIM_ESTAB__SANITIZE_UNDEFINED)
    target_enable_ubsan(sim_estab)
endif()
if(LIBSIM_ESTAB__SANITIZE_THREAD)
    target_enable_tsan(sim_estab)
endif()

# install
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sim_estab)
install(TARGETS sim_estab
    EXPORT libsim_estab
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME})
# Generate config file from template
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/libsim_estab-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libsim_estab-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libsim_estab
)
# Generate version file
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libsim_estab-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
# Install targets export (generates libsim_estab-targets.cmake)
install(EXPORT libsim_estab
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libsim_estab
    NAMESPACE libsim_estab::
    FILE libsim_estab-targets.cmake
)

# Install config files
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libsim_estab-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/libsim_estab-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libsim_estab
)

# install dep
# install(IMPORTED_RUNTIME_ARTIFACTS spdlog::spdlog
#     RUNTIME DESTINATION .
#     LIBRARY DESTINATION .)

# build tests if enabled
if(BUILD_TESTS)
    add_subdirectory(${project_path_prefix}/test/libsim_estab test)
endif()
