# Finds the BlocksRuntime library
# On Apple platforms, this does not exist and is folded into other System libraries

find_library(BLOCKS_RUNTIME NAMES BlocksRuntime
    PATHS ${SWIFT_LIBRARY_SEARCH_PATHS}
)
if (BLOCKS_RUNTIME)
    if (NOT TARGET BlocksRuntime::BlocksRuntime)
        add_library(BlocksRuntime::BlocksRuntime IMPORTED UNKNOWN)
        message(STATUS "Found BlocksRuntime: ${BLOCKS_RUNTIME}")
        cmake_path(GET BLOCKS_RUNTIME PARENT_PATH _BLOCKS_RUNTIME_DIR)
        set_target_properties(BlocksRuntime::BlocksRuntime PROPERTIES
                IMPORTED_LOCATION "${BLOCKS_RUNTIME}"
                INTERFACE_LINK_DIRECTORIES "${_BLOCKS_RUNTIME_DIR}"
                INTERFACE_COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C,CXX>:-fblocks>;SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc -fblocks>"
        )
    endif()
    set(BlocksRuntime_FOUND TRUE)
endif()
