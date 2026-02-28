include_guard()

function(target_enable_asan TARGET)
    if (MSVC)
        message(STATUS "${TARGET}: AddressSanitizer not enabled on MSVC")
        return()
    endif()

    message(STATUS "${TARGET}: enabling AddressSanitizer")
    target_compile_options(${TARGET} PRIVATE
        -fsanitize=address
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
    )
    target_link_options(${TARGET} PRIVATE
        -fsanitize=address
    )
endfunction()

function(target_enable_ubsan TARGET)
    if (MSVC)
        message(STATUS "${TARGET}: UndefinedBehaviorSanitizer not enabled on MSVC")
        return()
    endif()

    message(STATUS "${TARGET}: enabling UndefinedBehaviorSanitizer")
    target_compile_options(${TARGET} PRIVATE
        -fsanitize=undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET} PRIVATE
        -fsanitize=undefined
    )
endfunction()

function(target_enable_tsan TARGET)
    if (MSVC)
        message(STATUS "${TARGET}: ThreadSanitizer not enabled on MSVC")
        return()
    endif()

    message(STATUS "${TARGET}: enabling ThreadSanitizer")
    target_compile_options(${TARGET} PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET} PRIVATE
        -fsanitize=thread
    )
endfunction()
