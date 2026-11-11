function(add_lagom_library list item)
    list(FIND "${list}" "${item}" item_is_present)

    if (item_is_present EQUAL -1)
        set("${list}" "${${list}}" "${item}" PARENT_SCOPE)
     endif()
endfunction()

function(get_linked_lagom_libraries_impl target output)
    if (NOT TARGET "${target}")
        return()
    endif()

    get_property(visited GLOBAL PROPERTY _LADYBIRD_LINKED_LAGOM_LIBRARIES_VISITED)
    if ("${target}" IN_LIST visited)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY _LADYBIRD_LINKED_LAGOM_LIBRARIES_VISITED "${target}")

    get_target_property(target_is_imported "${target}" IMPORTED)
    if (target_is_imported)
        return()
    endif()

    get_target_property(target_type "${target}" TYPE)

    if ("${target_type}" STREQUAL "SHARED_LIBRARY")
        add_lagom_library("${output}" "${target}")
    elseif ("${target_type}" STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    get_target_property(target_libraries "${target}" LINK_LIBRARIES)

    foreach(target_library IN LISTS target_libraries)
        get_linked_lagom_libraries_impl("${target_library}" "${output}")
    endforeach()

    set("${output}" "${${output}}" PARENT_SCOPE)
endfunction()

function(get_linked_lagom_libraries target output)
    # Track all visited targets, including static libraries, separately from the shared libraries collected for installation.
    set_property(GLOBAL PROPERTY _LADYBIRD_LINKED_LAGOM_LIBRARIES_VISITED "")

    get_linked_lagom_libraries_impl(${target} ${output})
    set("${output}" "${${output}}" PARENT_SCOPE)
endfunction()
