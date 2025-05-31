include_guard()

include(CheckIPOSupported)

function(check_lto_support LTO_SUPPORTED)
    check_ipo_supported(RESULT supported)
    set(LTO_SUPPORTED ${supported} PARENT_SCOPE)
    if(supported)
        message(STATUS "IPO / LTO supported")
    else()
        message(STATUS "IPO / LTO not supported")
    endif()
endfunction()

function(target_enable_lto TARGET)
    message(STATUS "${TARGET}: IPO / LTO enabled")
    set_property(TARGET ${TARGET}
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endfunction()