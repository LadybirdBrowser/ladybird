include_guard()

include(clang_development)
include(rust_crate)

function(flap_layout_relevant_flags out_variable)
    set(kept_flags "")
    foreach (flag IN LISTS ARGN)
        # defines
        if (flag MATCHES "^[-/]([DUI])(.+)$")
            list(APPEND kept_flags "-${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
        # C++ version
        elseif (flag MATCHES "^[-/]std[:=](.+)$")
            list(APPEND kept_flags "-std=${CMAKE_MATCH_1}")
        # ABI (word size/baseline)
        elseif (flag MATCHES "^-m")
            list(APPEND kept_flags "${flag}")
        endif()
    endforeach()
    set(${out_variable} "${kept_flags}" PARENT_SCOPE)
endfunction()

function(flap_target_triple out_variable)
    if (DEFINED CACHE{FLAP_TARGET_TRIPLE})
        set(${out_variable} "${FLAP_TARGET_TRIPLE}" PARENT_SCOPE)
        return()
    endif()

    separate_arguments(compiler_arguments NATIVE_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
    set(triple "${CMAKE_CXX_COMPILER_TARGET}")
    if (NOT triple)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" ${compiler_arguments} -dumpmachine
            OUTPUT_VARIABLE triple
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if (NOT triple)
        # clang-cl ignores -dumpmachine, but names its target in the version banner anyway.
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" ${compiler_arguments} --version
            OUTPUT_VARIABLE version_banner
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if (version_banner MATCHES "Target: *([^\r\n]+)")
            set(triple "${CMAKE_MATCH_1}")
        endif()
    endif()
    if (NOT triple)
        message(FATAL_ERROR
            "Could not determine the target triple of ${CMAKE_CXX_COMPILER}, which flapc needs to read the LibJS layouts. Set CMAKE_CXX_COMPILER_TARGET."
        )
    endif()

    set(FLAP_TARGET_TRIPLE "${triple}" CACHE INTERNAL "Target triple flapc parses the LibJS headers for")
    set(${out_variable} "${triple}" PARENT_SCOPE)
endfunction()

function(flap_write_clang_arguments target output_path resource_directory)
    flap_target_triple(target_triple)

    set(arguments
        -xc++
        "-std=c++${CMAKE_CXX_STANDARD}"
        "-resource-dir=${resource_directory}"
        "--target=${target_triple}"
    )

    if (NOT MSVC)
        set(sysroot "${CMAKE_SYSROOT_COMPILE}")
        if (NOT sysroot)
            set(sysroot "${CMAKE_SYSROOT}")
        endif()
        if (sysroot)
            list(APPEND arguments "--sysroot=${sysroot}")
        endif()
        if (CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN)
            list(APPEND arguments "--gcc-toolchain=${CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN}")
        endif()
    endif()
    if (CMAKE_OSX_SYSROOT)
        list(APPEND arguments -isysroot "${CMAKE_OSX_SYSROOT}")
    endif()
    if (CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 osx_architecture)
        list(APPEND arguments -arch "${osx_architecture}")
    endif()

    set(compiler_flags "${CMAKE_CXX_COMPILER_ARG1} ${CMAKE_CXX_FLAGS}")
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
    if (build_type)
        string(APPEND compiler_flags " ${CMAKE_CXX_FLAGS_${build_type}}")
    endif()
    separate_arguments(compiler_flags NATIVE_COMMAND "${compiler_flags}")
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$" AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        list(APPEND arguments ${compiler_flags})
    else()
        flap_layout_relevant_flags(compiler_flags ${compiler_flags})
        list(APPEND arguments ${compiler_flags})
    endif()

    set(system_includes
        ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
        ${CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES}
    )
    list(REMOVE_DUPLICATES system_includes)
    foreach (system_include IN LISTS system_includes)
        list(APPEND arguments -isystem "${system_include}")
    endforeach()

    foreach (sanitizer address memory undefined)
        string(TOUPPER "${sanitizer}" sanitizer_option)
        if (ENABLE_${sanitizer_option}_SANITIZER)
            list(APPEND arguments "-fsanitize=${sanitizer}")
        endif()
    endforeach()

    list(APPEND arguments
        # We need to know offsets of private/protected members, so disable access control.
        -Xclang -fno-access-control
        -Wno-error
    )

    list(JOIN arguments "\n" arguments)
    set(definitions "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>")
    set(includes "$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>")
    file(GENERATE
        OUTPUT "${output_path}"
        CONTENT
"${arguments}
$<$<BOOL:${definitions}>:-D$<JOIN:${definitions},\n-D>\n>\
$<$<BOOL:${includes}>:-I$<JOIN:${includes},\n-I>\n>"
        CONDITION $<COMPILE_LANGUAGE:CXX>
        TARGET ${target}
    )
endfunction()

macro(generate_flap_interpreter target)
    if ("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^(x86_64|amd64|AMD64)$")
        set(FLAP_ARCH "x86_64")
    elseif ("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^(aarch64|arm64|ARM64)$")
        set(FLAP_ARCH "aarch64")
    else()
        message(FATAL_ERROR "LibJS requires JS interpreter support for ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    enable_language(ASM)
    if (WIN32 AND CMAKE_ASM_COMPILER_ID MATCHES "Clang")
        set(CMAKE_ASM_COMPILE_OPTIONS_MSVC_DEBUG_INFORMATION_FORMAT_ProgramDatabase "")
        set(CMAKE_ASM_COMPILE_OPTIONS_MSVC_DEBUG_INFORMATION_FORMAT_EditAndContinue "")
    endif()

    set(FLAP_DSL "${CMAKE_CURRENT_SOURCE_DIR}/Interpreter/interpreter.flap")
    set(FLAP_LAYOUT_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/Interpreter/layout.spec")
    set(FLAP_GENERATED_S "${CMAKE_CURRENT_BINARY_DIR}/Interpreter/interpreter_${FLAP_ARCH}.S")
    set(FLAP_CLANG_ARGS_FILE "${CMAKE_CURRENT_BINARY_DIR}/Interpreter/layout-clang-args.txt")

    # flapc is a host tool even when the target is cross-compiled, and it loads libclang to read the layouts out of the headers.
    find_libclang(flap_libclang flap_libclang_resource_directory)
    flap_write_clang_arguments(${target} "${FLAP_CLANG_ARGS_FILE}" "${flap_libclang_resource_directory}")

    get_filename_component(flap_libclang_directory "${flap_libclang}" DIRECTORY)
    build_rust_binary(
        MANIFEST_PATH Flap/Cargo.toml
        CRATE_NAME flapc
        BINARY_NAME flapc
        OUTPUT_PATH_VAR FLAPC_BIN
        HOST
        EXTRA_ENV "LIBCLANG_LIB_DIR=${flap_libclang_directory}"
    )

    if (WIN32)
        set(FLAP_OBJECT_FORMAT "coff")
    elseif (APPLE)
        set(FLAP_OBJECT_FORMAT "macho")
    else()
        set(FLAP_OBJECT_FORMAT "elf")
    endif()

    set(FLAP_EXTRA_FLAGS "")
    if (CMAKE_BUILD_TYPE STREQUAL "Debug"
        OR ENABLE_ADDRESS_SANITIZER
        OR ENABLE_MEMORY_SANITIZER
        OR ENABLE_UNDEFINED_SANITIZER)
        list(APPEND FLAP_EXTRA_FLAGS "--enable-assertions")
    endif()
    # All Apple Silicon chips are ARMv8.5+ which includes FEAT_JSCVT.
    if ("${FLAP_ARCH}" STREQUAL "aarch64" AND APPLE)
        list(APPEND FLAP_EXTRA_FLAGS "--has-jscvt")
    endif()

    add_custom_command(
        OUTPUT "${FLAP_GENERATED_S}"
        COMMAND "${FLAPC_BIN}" --arch ${FLAP_ARCH}
            --object-format ${FLAP_OBJECT_FORMAT}
            --layout-spec "${FLAP_LAYOUT_SPEC}"
            --clang-args "${FLAP_CLANG_ARGS_FILE}"
            --input "${FLAP_DSL}"
            --output "${FLAP_GENERATED_S}"
            ${FLAP_EXTRA_FLAGS}
        DEPENDS "${FLAPC_BIN}" "${FLAP_DSL}"
            "${FLAP_LAYOUT_SPEC}" "${FLAP_CLANG_ARGS_FILE}"
        DEPFILE "${FLAP_GENERATED_S}.d"
        COMMENT "Generating interpreter_${FLAP_ARCH}.S from Flap"
    )

    target_sources(${target} PRIVATE "${FLAP_GENERATED_S}")
    if ("${FLAP_ARCH}" STREQUAL "x86_64" AND CMAKE_ASM_COMPILER_ID MATCHES "Clang")
        # Keep conditional branches within 32-byte boundaries so Intel's
        # microcode mitigation does not prevent their blocks from using the DSB.
        set_property(SOURCE "${FLAP_GENERATED_S}" APPEND PROPERTY COMPILE_OPTIONS
            "-malign-branch-boundary=32;-malign-branch=fused,jcc")
    endif()
endmacro()
