cmake_minimum_required (VERSION 3.21)

# sanity check
if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "Building in-source is not supported! Create a build dir and remove ${CMAKE_SOURCE_DIR}/CMakeCache.txt")
endif()

# import early cmake module
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# project 
project(libsim_estab)

# import cmake module
enable_language(CXX)

include(GNUInstallDirs)
include(CMakePrintHelpers)

include(util/expr_util)
include(dep)
include(cpp_standard)
include(warning)
include(lto)

# option

# common path
cmake_path(GET CMAKE_CURRENT_LIST_FILE PARENT_PATH project_path_current)
cmake_path(GET project_path_current PARENT_PATH project_path_prefix)
message(STATUS "${PROJECT_NAME}: project path prefix: ${project_path_prefix}")
message(STATUS "${PROJECT_NAME}: project path: ${project_path_current}")

# dependency
find_package(Boost REQUIRED COMPONENTS headers filesystem nowide)
message(STATUS "${PROJECT_NAME}: use boost: ${Boost_DIR}")

find_package(fmt REQUIRED)
message(STATUS "${PROJECT_NAME}: use fmt: ${fmt_DIR}")

find_package(spdlog REQUIRED)
message(STATUS "${PROJECT_NAME}: use spdlog: ${spdlog_DIR}")

find_package(range-v3 REQUIRED)
message(STATUS "${PROJECT_NAME}: use range-v3: ${range-v3_DIR}")

find_package(tsl-robin-map REQUIRED)
message(STATUS "${PROJECT_NAME}: use tsl-robin-map: ${tsl-robin-map_DIR}")

find_package (Eigen3 3.3 REQUIRED NO_MODULE)
message(STATUS "${PROJECT_NAME}: use Eigen3: ${Eigen3_DIR}")

## dep: config
find_package(nlohmann_json 3.11.0 REQUIRED)
message(STATUS "${PROJECT_NAME}: use nlohmann_json: ${nlohmann_json_DIR}")

## dep: bundled, to be built together
message(STATUS "${PROJECT_NAME}: use bundled project prefix: ${project_path_prefix}")

# compile & link property
check_lto_support(LTO_SUPPORTED)

# main library
add_library(sim_estab SHARED)
add_library(libsim_estab::sim_estab ALIAS sim_estab)
set_target_properties(sim_estab PROPERTIES CXX_STANDARD_REQUIRED ON)
set_target_properties(sim_estab PROPERTIES CXX_STANDARD 23)
set_target_properties(sim_estab PROPERTIES CXX_EXTENSION ON)
target_sources(sim_estab PRIVATE
    # impl
    src/lib.cpp
)
target_link_libraries(sim_estab PRIVATE 
    range-v3::range-v3
    fmt::fmt
    tsl::robin_map)
target_include_directories(sim_estab PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include/sim_estab>)
set_target_properties(sim_estab PROPERTIES
    INSTALL_RPATH "\$ORIGIN")
# TODO: change symbol visibility in the future
set_target_properties(sim_estab PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

# install
install(DIRECTORY include/ DESTINATION include/sim_estab)
install(TARGETS sim_estab
    EXPORT libsim_estab
    RUNTIME DESTINATION lib
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
install(EXPORT libsim_estab
    DESTINATION lib/cmake/libsim_estab
    NAMESPACE libsim_estab::
    FILE libsim_estab-config.cmake)

# install dep
# install(IMPORTED_RUNTIME_ARTIFACTS spdlog::spdlog
#     RUNTIME DESTINATION .
#     LIBRARY DESTINATION .)
