#
# ccache setup
#

list(APPEND COMPILERS
    "CMAKE_C_COMPILER"
    "CMAKE_CXX_COMPILER"
)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    foreach(compiler ${COMPILERS})
        get_filename_component(compiler_path "${${compiler}}" REALPATH)
        get_filename_component(compiler_name "${compiler_path}" NAME)
        if (NOT ${compiler_name} MATCHES "ccache")
            set("${compiler}_LAUNCHER" "${CCACHE_PROGRAM}" CACHE FILEPATH "Path to a compiler launcher program, e.g. ccache")
        endif()
    endforeach()
endif()
