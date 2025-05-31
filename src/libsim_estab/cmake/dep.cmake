include_guard()

include(util/expr_util)

function(dep_get_dll_path target target_dll_name output)
    get_target_property(interface_link_dir_list ${target} INTERFACE_LINK_DIRECTORIES)
    set(dll_path "")
    foreach(_loc ${interface_link_dir_list})
        split_generator_expression_condition(${_loc} _loc_cond _loc_val)
        if(_loc_val)
            # handle cond generator expr
            cmake_path(GET _loc_val PARENT_PATH _loc_val_dll)
            cmake_path(APPEND _loc_val_dll "bin")
            cmake_path(APPEND _loc_val_dll ${target_dll_name})
            string(CONCAT _loc_dll "$<" ${_loc_cond} ":" ${_loc_val_dll} ">")
            list(APPEND dll_path ${_loc_dll})
        else()
            # TODO
            message(FATAL_ERROR "TODO")
        endif()
    endforeach()
    set(${output} ${dll_path} PARENT_SCOPE)
endfunction()