include_guard()

function(is_generator_expression str result)
    string(SUBSTRING "${str}" 0 2 start_chars)
    string(SUBSTRING "${str}" -1 1 end_char)

    if("${start_chars}" STREQUAL "$<" AND "${end_char}" STREQUAL ">")
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(split_generator_expression_condition expr expr_cond expr_val)
    string(REGEX MATCHALL  "^\\$<(\\$<.*>):(.*)>$" match "${expr}")
    if(match)
        set(${expr_cond} ${CMAKE_MATCH_1} PARENT_SCOPE) # set the condition
        set(${expr_val}  ${CMAKE_MATCH_2} PARENT_SCOPE) # set the value
    else()
        set(${expr_cond} "" PARENT_SCOPE)
        set(${expr_val}  "" PARENT_SCOPE)
    endif()
endfunction()

function(concat_generator_expression_condition expr_cond expr_val expr_out)

endfunction()