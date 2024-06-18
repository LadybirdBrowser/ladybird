include(${CMAKE_CURRENT_LIST_DIR}/common_compile_options.cmake)

add_compile_options(-Wno-maybe-uninitialized)
add_compile_options(-Wno-shorten-64-to-32)

if(NOT MSVC)
    add_compile_options(-fsigned-char)
    add_compile_options(-ggnu-pubnames)
else()
    # char is signed
    add_compile_options(/J)
    # full symbolic debugginng information
    add_compile_options(/Z7)
endif()

if (NOT WIN32)
    add_compile_options(-fPIC)
endif()

if (LINUX)
    add_compile_options(-D_FILE_OFFSET_BITS=64)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (NOT MSVC)
        add_compile_options(-ggdb3)
    endif()
    add_compile_options(-Og)
else()
    add_compile_options(-O2)
    add_compile_options(-g1)
endif()

function(add_linker_flag_if_supported flag)
    include(CheckLinkerFlag)

    check_linker_flag(CXX ${flag} LAGOM_LINKER_SUPPORTS_${flag})
    if (${LAGOM_LINKER_SUPPORTS_${flag}})
        add_link_options(${flag})
    endif()
endfunction()

add_linker_flag_if_supported(LINKER:--gdb-index)

if (NOT ENABLE_FUZZERS)
    add_linker_flag_if_supported(LINKER:-Bsymbolic-non-weak-functions)
endif()

add_link_options(LINKER:--no-undefined)

if (ENABLE_LAGOM_COVERAGE_COLLECTION)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$" AND NOT ENABLE_FUZZERS)
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate)
    else()
        message(FATAL_ERROR
            "Collecting code coverage is unsupported in this configuration.")
    endif()
endif()
