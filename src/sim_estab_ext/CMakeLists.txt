cmake_minimum_required(VERSION 3.24...3.32)

project(sim_estab_ext)

message(STATUS "${libsim_estab_DIR}")
find_package(libsim_estab REQUIRED)
message(STATUS "use libsim_estab: ${libsim_estab_DIR}")

# python
if(MSVC)
    find_package(PythonInterp)
    find_package(Python)
endif()
find_package(Python COMPONENTS Interpreter Development.Module
    OPTIONAL_COMPONENTS Development.SABIModule)
message(STATUS "use Python: ${Python_EXECUTABLE} ${Python_VERSION_STRING}")

# nanobind
find_package(nanobind REQUIRED)
message(STATUS "use nanobind: ${nanobind_DIR}")

# dependency
find_package(Boost REQUIRED COMPONENTS headers filesystem nowide log)
message(STATUS "${PROJECT_NAME}: use boost: ${Boost_DIR}")
# get_target_property(boost_headers_incs Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
# message(STATUS "Boost::headers INTERFACE_INCLUDE_DIRECTORIES = ${boost_headers_incs}")
# get_target_property(boost_log_incs Boost::log INTERFACE_INCLUDE_DIRECTORIES)
# message(STATUS "Boost::log INTERFACE_INCLUDE_DIRECTORIES = ${boost_log_incs}")

find_package(fmt REQUIRED)
message(STATUS "${PROJECT_NAME}: use fmt: ${fmt_DIR}")

find_package(rang REQUIRED)
message(STATUS "${PROJECT_NAME}: use rang: ${rang_DIR}")

find_package(range-v3 REQUIRED)
message(STATUS "${PROJECT_NAME}: use range-v3: ${range-v3_DIR}")

find_package(tsl-robin-map REQUIRED)
message(STATUS "${PROJECT_NAME}: use tsl-robin-map: ${tsl-robin-map_DIR}")

# nanobind extension
nanobind_add_module(
    _if
    NB_STATIC LTO
    # STABLE_ABI
    FREE_THREADED
    bind.cpp)
set_target_properties(_if PROPERTIES CXX_STANDARD_REQUIRED ON)
set_target_properties(_if PROPERTIES CXX_STANDARD 23)
set_target_properties(_if PROPERTIES CXX_EXTENSION ON)
target_compile_features(_if PUBLIC cxx_std_23)
# target_compile_definitions(_if PRIVATE BOOST_ALL_DYN_LINK) # Not sure why nanobind needs explicit definition of this
target_link_libraries(_if PRIVATE Boost::headers Boost::log)
target_link_libraries(_if PRIVATE libsim_estab::sim_estab)
if (DEFINED DESIGNATED_RPATH)
    message(STATUS "use designated rpath ${DESIGNATED_RPATH}")
    set_target_properties(_if PROPERTIES
        INSTALL_RPATH "${DESIGNATED_RPATH}")
else()
    set_target_properties(_if PROPERTIES
        INSTALL_RPATH "\$ORIGIN")
endif()

install(TARGETS _if LIBRARY DESTINATION .)
