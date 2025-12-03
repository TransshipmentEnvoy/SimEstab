include_guard()

include(CheckIPOSupported)

function(check_lto_support LTO_SUPPORTED)
    check_ipo_supported(RESULT supported OUTPUT output)
    set(LTO_SUPPORTED ${supported} PARENT_SCOPE)
    if(supported)
        message(STATUS "IPO / LTO supported")
    else()
        message(STATUS "IPO / LTO not supported: ${output}")
    endif()
endfunction()

# Enable LTO for all build types
function(target_enable_lto TARGET)
    message(STATUS "${TARGET}: IPO / LTO enabled")
    set_property(TARGET ${TARGET}
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endfunction()

# Enable LTO only for Release-related build types (Release, RelWithDebInfo, MinSizeRel)
# This function properly handles both single-config and multi-config generators
function(target_enable_lto_release_only TARGET)
    # For multi-config generators (Ninja Multi-Config, Visual Studio, Xcode, etc.)
    # we need to set per-config properties
    get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

    if(is_multi_config)
        # Multi-config generator: set per-configuration properties
        set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION_DEBUG FALSE)
        message(STATUS "${TARGET}: IPO / LTO enabled for Release/RelWithDebInfo/MinSizeRel (multi-config)")
    else()
        # Single-config generator: check CMAKE_BUILD_TYPE
        if(CMAKE_BUILD_TYPE STREQUAL "Release" OR
           CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR
           CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
            set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            message(STATUS "${TARGET}: IPO / LTO enabled (single-config, ${CMAKE_BUILD_TYPE})")
        else()
            set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
            message(STATUS "${TARGET}: IPO / LTO disabled (single-config, ${CMAKE_BUILD_TYPE})")
        endif()
    endif()
endfunction()