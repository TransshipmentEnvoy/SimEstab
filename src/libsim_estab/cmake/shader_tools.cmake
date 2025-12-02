include_guard()

# Find shader compilation tools: glslang and spirv-cross
# This module provides functions to locate binary tools for GLSL to SPIR-V compilation
# Prioritizes Conan-provided packages, falls back to system installations
#
# Usage:
#   include(shader_tools)
#   find_package(glslang CONFIG)        # optional, explicit find for Conan
#   find_package(spirv-cross CONFIG)    # optional, explicit find for Conan
#   find_shader_tools()                 # call after find_package
#
# After calling find_shader_tools(), the following variables are set:
#   GLSLANG_VALIDATOR - path to glslangValidator executable
#   SPIRV_CROSS       - path to spirv-cross executable

# Quietly try to find packages at include time (in case user forgets)
# Don't specify CONFIG to allow both CONFIG and MODULE mode
find_package(glslang QUIET)
find_package(spirv-cross QUIET)

function(find_shader_tools)
    # Determine the correct package folder based on build type (for Conan)
    set(_glslang_package_folder "")
    set(_spirv_cross_package_folder "")

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(glslang_PACKAGE_FOLDER_DEBUG)
            set(_glslang_package_folder "${glslang_PACKAGE_FOLDER_DEBUG}")
        endif()
        if(spirv-cross_PACKAGE_FOLDER_DEBUG)
            set(_spirv_cross_package_folder "${spirv-cross_PACKAGE_FOLDER_DEBUG}")
        endif()
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        if(glslang_PACKAGE_FOLDER_RELEASE)
            set(_glslang_package_folder "${glslang_PACKAGE_FOLDER_RELEASE}")
        endif()
        if(spirv-cross_PACKAGE_FOLDER_RELEASE)
            set(_spirv_cross_package_folder "${spirv-cross_PACKAGE_FOLDER_RELEASE}")
        endif()
    elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        if(glslang_PACKAGE_FOLDER_RELWITHDEBINFO)
            set(_glslang_package_folder "${glslang_PACKAGE_FOLDER_RELWITHDEBINFO}")
        endif()
        if(spirv-cross_PACKAGE_FOLDER_RELWITHDEBINFO)
            set(_spirv_cross_package_folder "${spirv-cross_PACKAGE_FOLDER_RELWITHDEBINFO}")
        endif()
    else()
        # Fallback: try debug first, then release
        if(glslang_PACKAGE_FOLDER_DEBUG)
            set(_glslang_package_folder "${glslang_PACKAGE_FOLDER_DEBUG}")
        elseif(glslang_PACKAGE_FOLDER_RELEASE)
            set(_glslang_package_folder "${glslang_PACKAGE_FOLDER_RELEASE}")
        endif()
        if(spirv-cross_PACKAGE_FOLDER_DEBUG)
            set(_spirv_cross_package_folder "${spirv-cross_PACKAGE_FOLDER_DEBUG}")
        elseif(spirv-cross_PACKAGE_FOLDER_RELEASE)
            set(_spirv_cross_package_folder "${spirv-cross_PACKAGE_FOLDER_RELEASE}")
        endif()
    endif()

    # Find glslangValidator (glslang compiler)
    # First try Conan package path
    if(_glslang_package_folder)
        find_program(_GLSLANG_VALIDATOR
            NAMES glslangValidator glslang
            HINTS "${_glslang_package_folder}/bin"
            NO_DEFAULT_PATH
            DOC "Path to glslangValidator executable"
        )
    endif()
    # Fallback to system paths if not found in Conan
    if(NOT _GLSLANG_VALIDATOR)
        find_program(_GLSLANG_VALIDATOR
            NAMES glslangValidator glslang
            HINTS
                $ENV{VULKAN_SDK}/bin
                $ENV{VULKAN_SDK}/Bin
            PATHS
                /usr/bin
                /usr/local/bin
                /opt/vulkan/bin
            DOC "Path to glslangValidator executable"
        )
    endif()

    # Find spirv-cross
    # First try Conan package path
    if(_spirv_cross_package_folder)
        find_program(_SPIRV_CROSS
            NAMES spirv-cross
            HINTS "${_spirv_cross_package_folder}/bin"
            NO_DEFAULT_PATH
            DOC "Path to spirv-cross executable"
        )
    endif()
    # Fallback to system paths if not found in Conan
    if(NOT _SPIRV_CROSS)
        find_program(_SPIRV_CROSS
            NAMES spirv-cross
            HINTS
                $ENV{VULKAN_SDK}/bin
                $ENV{VULKAN_SDK}/Bin
            PATHS
                /usr/bin
                /usr/local/bin
                /opt/vulkan/bin
            DOC "Path to spirv-cross executable"
        )
    endif()

    # Export to parent scope
    set(GLSLANG_VALIDATOR "${_GLSLANG_VALIDATOR}" PARENT_SCOPE)
    set(SPIRV_CROSS "${_SPIRV_CROSS}" PARENT_SCOPE)

    # Report findings
    if(_GLSLANG_VALIDATOR)
        message(STATUS "${PROJECT_NAME}: found glslangValidator program: ${_GLSLANG_VALIDATOR}")
    else()
        message(WARNING "${PROJECT_NAME}: glslangValidator program not found - shader compilation will not be available")
    endif()

    if(_SPIRV_CROSS)
        message(STATUS "${PROJECT_NAME}: found spirv-cross program: ${_SPIRV_CROSS}")
    else()
        message(WARNING "${PROJECT_NAME}: spirv-cross program not found - SPIR-V cross-compilation will not be available")
    endif()
endfunction()

# Optional: Define a function to compile GLSL to SPIR-V
# Usage: compile_glsl_to_spirv(TARGET target_name SOURCES shader1.vert shader2.frag OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
function(compile_glsl_to_spirv)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "TARGET;OUTPUT_DIR" "SOURCES")

    if(NOT GLSLANG_VALIDATOR)
        message(FATAL_ERROR "glslangValidator not found, cannot compile shaders")
    endif()

    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    endif()

    file(MAKE_DIRECTORY ${ARG_OUTPUT_DIR})

    set(SPIRV_OUTPUTS)
    foreach(SHADER ${ARG_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPIRV_OUTPUT "${ARG_OUTPUT_DIR}/${SHADER_NAME}.spv")
        
        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SPIRV_OUTPUT}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
            VERBATIM
        )
        list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
    endforeach()

    if(ARG_TARGET)
        add_custom_target(${ARG_TARGET} DEPENDS ${SPIRV_OUTPUTS})
    endif()
endfunction()
