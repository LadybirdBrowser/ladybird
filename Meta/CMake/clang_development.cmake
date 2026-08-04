include_guard()

# find zstd package before clang so that it is built fully and not
# specifically for clang's use. Clang/LLVM requires zstd and it's cmake
# will build zstd if not available, but clang's cmake will incorrectly
# build zstd for curl 8.18.0, which also depends on zstd.
#
# see https://github.com/LadybirdBrowser/ladybird/pull/7738
# related to https://github.com/llvm/llvm-project/issues/139666
macro(_find_zstd_before_clang)
    find_package(zstd QUIET CONFIG REQUIRED)
endmacro()

# Asks a clang driver for the directory holding its builtin headers. Anything that
# is not a GCC-style clang, which is to say GCC and clang-cl, answers nothing.
function(_print_clang_resource_directory driver arguments out_variable)
    execute_process(
        COMMAND "${driver}" ${arguments} -print-resource-dir
        OUTPUT_VARIABLE resource_directory
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if (NOT EXISTS "${resource_directory}/include/stddef.h")
        set(resource_directory "")
    endif()
    set(${out_variable} "${resource_directory}" PARENT_SCOPE)
endfunction()

# Finds the clang and llvm development packages that match the compiler in use.
# A clang plugin is loaded by that exact compiler, so nothing else will do.
macro(find_clang_development)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
        set(DESIRED_CLANG_VERSION "${CMAKE_CXX_COMPILER_VERSION}")

        _find_zstd_before_clang()

        find_package(Clang "${DESIRED_CLANG_VERSION}" QUIET REQUIRED CONFIG)
        find_package(LLVM "${DESIRED_CLANG_VERSION}" QUIET REQUIRED CONFIG)
    endif()
endmacro()

# Looks for the clang resource directory, holding the builtin headers, next to a
# toolchain's libraries. Distributions lay this out as <libdir>/clang/<version>,
# either alongside the shared libraries or one level up from a versioned prefix.
function(_find_clang_resource_directory library_directory out_variable)
    set(search_directories "${library_directory}")
    get_filename_component(parent_library_directory "${library_directory}/../lib" ABSOLUTE)
    list(APPEND search_directories "${parent_library_directory}")
    list(REMOVE_DUPLICATES search_directories)

    set(candidates "")
    foreach (search_directory IN LISTS search_directories)
        file(GLOB directory_candidates "${search_directory}/clang/*")
        list(APPEND candidates ${directory_candidates})
    endforeach()

    # Prefer the newest resource directory.
    list(SORT candidates COMPARE NATURAL ORDER DESCENDING)
    foreach (candidate IN LISTS candidates)
        if (EXISTS "${candidate}/include/stddef.h")
            set(${out_variable} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_variable} "" PARENT_SCOPE)
endfunction()

# Finds the libclang shared library, and the resource directory that goes with it,
# for build tools that parse this tree's headers.
function(find_libclang library_variable resource_directory_variable)
    set(library_hints "")
    set(resource_directory "")

    # A clang driver knows where its own resources are, and the libclang installed
    # beside them is the one that reads headers the way the driver does. Prefer the
    # compiler building the tree, then any clang the machine has: GCC and the
    # MSVC-style driver do not answer -print-resource-dir, and a cross-compiler may
    # not ship a libclang at all.
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
        separate_arguments(compiler_arguments NATIVE_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
        _print_clang_resource_directory("${CMAKE_CXX_COMPILER}" "${compiler_arguments}" resource_directory)
    endif()
    if (NOT resource_directory)
        find_program(LIBCLANG_CLANG_DRIVER NAMES clang NO_CMAKE_FIND_ROOT_PATH
            DOC "clang driver asked where libclang's resource directory is")
        if (LIBCLANG_CLANG_DRIVER)
            _print_clang_resource_directory("${LIBCLANG_CLANG_DRIVER}" "" resource_directory)
        endif()
    endif()
    if (resource_directory)
        # <prefix>/lib/clang/<version> holds the resources, <prefix>/lib the libraries.
        get_filename_component(library_hint "${resource_directory}/../.." ABSOLUTE)
        list(APPEND library_hints "${library_hint}")
    endif()

    if (CMAKE_HOST_APPLE)
        execute_process(
            COMMAND xcrun --find clang
            OUTPUT_VARIABLE host_clang
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if (host_clang)
            get_filename_component(host_toolchain_library_directory "${host_clang}/../../lib" ABSOLUTE)
            list(APPEND library_hints "${host_toolchain_library_directory}")
        endif()
    endif()

    find_library(LIBCLANG_LIBRARY
        NAMES clang libclang
        HINTS ${library_hints}
        DOC "Path to the libclang shared library"
        REQUIRED
        NO_CMAKE_FIND_ROOT_PATH
    )

    if (NOT LIBCLANG_RESOURCE_DIR)
        get_filename_component(library_directory "${LIBCLANG_LIBRARY}" DIRECTORY)
        if (NOT resource_directory)
            _find_clang_resource_directory("${library_directory}" resource_directory)
        endif()
        if (NOT resource_directory)
            message(FATAL_ERROR
                "Found ${LIBCLANG_LIBRARY} but no clang resource directory to go with it.\n"
                "Set LIBCLANG_RESOURCE_DIR to the directory containing \"include/stddef.h\"."
            )
        endif()
        set(LIBCLANG_RESOURCE_DIR "${resource_directory}" CACHE PATH "Clang resource directory used by libclang")
    endif()

    set(${library_variable} "${LIBCLANG_LIBRARY}" PARENT_SCOPE)
    set(${resource_directory_variable} "${LIBCLANG_RESOURCE_DIR}" PARENT_SCOPE)
endfunction()
