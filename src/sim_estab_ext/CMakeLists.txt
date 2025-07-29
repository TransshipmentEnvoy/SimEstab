cmake_minimum_required(VERSION 3.24...3.32)

project(sim_estab_ext)

find_package(libsim_estab REQUIRED)
message(STATUS "use libsim_estab: ${libsim_estab_DIR}")

# python
if(MSVC)
    find_package(PythonInterp)
    find_package(Python)
endif()
find_package(Python COMPONENTS Interpreter Development REQUIRED)
message(STATUS "use Python: ${Python_EXECUTABLE} ${Python_VERSION_STRING}")

# nanobind
find_package(nanobind REQUIRED)
message(STATUS "use nanobind: ${nanobind_DIR}")

# nanobind extension
nanobind_add_module(
    _if
    NB_STATIC STABLE_ABI LTO
    bind.cpp)
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
