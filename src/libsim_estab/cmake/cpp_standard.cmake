include_guard()

include(CheckCXXCompilerFlag)

# C++26 is the project-wide standard. The Conan profiles already select gnu26
# (buildsys/conan/profile/linux_*); this keeps CMake from overriding them.
set(LIBSIM_ESTAB__CXX_STANDARD 26)

# GCC gates P2996 reflection behind -freflection; it is not implied by -std=c++26.
check_cxx_compiler_flag(-freflection LIBSIM_ESTAB__HAS_FREFLECTION)

# Sets the standard, GNU extensions and reflection on a target.
#
# -freflection is attached PUBLIC on purpose: the library exports its API as
# C++20 module interface units via FILE_SET CXX_MODULES, and consumers recompile
# those .cppm units themselves. A PRIVATE flag would break every consumer as soon
# as reflection appears in an interface.
function(target_set_cxx_standard target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD ${LIBSIM_ESTAB__CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON
    )
    target_compile_features(${target_name} PUBLIC cxx_std_${LIBSIM_ESTAB__CXX_STANDARD})

    if(LIBSIM_ESTAB__HAS_FREFLECTION)
        target_compile_options(${target_name} PUBLIC
            $<$<COMPILE_LANGUAGE:CXX>:-freflection>)
    endif()
endfunction()
