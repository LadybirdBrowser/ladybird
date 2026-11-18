#
# ccache setup
#

list(APPEND COMPILERS
    "CMAKE_C_COMPILER"
    "CMAKE_CXX_COMPILER"
)
if (LADYBIRD_ENABLE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
endif()
foreach(compiler ${COMPILERS})
    if (LADYBIRD_ENABLE_CCACHE AND CCACHE_PROGRAM)
        get_filename_component(compiler_path "${${compiler}}" REALPATH)
        get_filename_component(compiler_name "${compiler_path}" NAME)
        if (NOT ${compiler_name} MATCHES "ccache")
            set("${compiler}_LAUNCHER" "${CCACHE_PROGRAM}" CACHE FILEPATH "Path to a compiler launcher program, e.g. ccache")
        endif()
    elseif (NOT LADYBIRD_ENABLE_CCACHE AND DEFINED CACHE{${compiler}_LAUNCHER})
        string(REGEX MATCH "^[^;]*" launcher_program "$CACHE{${compiler}_LAUNCHER}")
        get_filename_component(launcher_name "${launcher_program}" NAME)
        if (launcher_name STREQUAL "ccache" OR launcher_name STREQUAL "ccache.exe"
            OR (CCACHE_PROGRAM AND launcher_program STREQUAL CCACHE_PROGRAM))
            unset("${compiler}_LAUNCHER" CACHE)
        endif()
    endif()
endforeach()
