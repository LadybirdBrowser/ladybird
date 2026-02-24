if (ENABLE_TRACY OR ENABLE_TRACY_MEMORY)
    find_package(Tracy CONFIG REQUIRED)
    add_compile_definitions(TRACY_ENABLE)

    if (ENABLE_TRACY_MEMORY)
        add_compile_definitions(TRACY_ENABLE_MEMORY)
    endif()

    if (TRACY_CALLSTACK_DEPTH GREATER 0)
        add_compile_definitions(TRACY_CALLSTACK=${TRACY_CALLSTACK_DEPTH})
    endif()

    if (NOT WIN32)
        add_cxx_compile_options(-fno-omit-frame-pointer)
    endif()

    # Tracy needs full debug info to resolve call stack symbols.
    if (NOT WIN32)
        add_cxx_compile_options(-g2)
    else()
        add_cxx_compile_options(/Z7)
    endif()

    if (NOT APPLE AND NOT WIN32)
        # Export symbols from executables so Tracy can resolve call stack addresses.
        add_link_options(-rdynamic)
    endif()

    link_libraries(Tracy::TracyClient)
endif()
