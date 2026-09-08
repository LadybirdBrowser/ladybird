include_guard()

include(rust_crate)

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
    set(FLAP_LAYOUT_PROBE_CPP "${CMAKE_CURRENT_BINARY_DIR}/Interpreter/layout-probe.cpp")
    set(FLAP_LAYOUT_PROBE_TARGET "${target}-flap-layout-probe")

    build_rust_binary(
        MANIFEST_PATH Flap/Cargo.toml
        CRATE_NAME flapc
        BINARY_NAME flapc
        OUTPUT_PATH_VAR FLAPC_BIN
        HOST
    )

    add_custom_command(
        OUTPUT "${FLAP_LAYOUT_PROBE_CPP}"
        COMMAND "${FLAPC_BIN}"
            --layout-spec "${FLAP_LAYOUT_SPEC}"
            --emit-layout-probe "${FLAP_LAYOUT_PROBE_CPP}"
        DEPENDS "${FLAPC_BIN}" "${FLAP_LAYOUT_SPEC}"
        COMMENT "Generating LibJS layout probe"
    )

    add_library(${FLAP_LAYOUT_PROBE_TARGET} OBJECT "${FLAP_LAYOUT_PROBE_CPP}")
    set_source_files_properties("${FLAP_LAYOUT_PROBE_CPP}" PROPERTIES GENERATED TRUE)
    # Make sure it's an actual object file and not LTO-poisoned bitcode or whatever the compiler feels like generating.
    set_target_properties(${FLAP_LAYOUT_PROBE_TARGET} PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
    target_compile_definitions(${FLAP_LAYOUT_PROBE_TARGET} PRIVATE "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>")
    target_compile_options(${FLAP_LAYOUT_PROBE_TARGET} PRIVATE "$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>")
    target_include_directories(${FLAP_LAYOUT_PROBE_TARGET} PRIVATE "$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>")
    target_link_libraries(${FLAP_LAYOUT_PROBE_TARGET} PRIVATE "$<TARGET_PROPERTY:${target},LINK_LIBRARIES>")
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${FLAP_LAYOUT_PROBE_TARGET} PRIVATE /clang:-fno-access-control /clang:-Wno-error)
    else()
        target_compile_options(${FLAP_LAYOUT_PROBE_TARGET} PRIVATE -fno-access-control -Wno-error)
    endif()

    if (NOT CMAKE_OBJDUMP)
        find_program(CMAKE_OBJDUMP NAMES llvm-objdump objdump REQUIRED)
    endif()

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
            --layout-object "$<TARGET_OBJECTS:${FLAP_LAYOUT_PROBE_TARGET}>"
            --objdump "${CMAKE_OBJDUMP}"
            --input "${FLAP_DSL}"
            --output "${FLAP_GENERATED_S}"
            ${FLAP_EXTRA_FLAGS}
        DEPENDS "${FLAPC_BIN}" "${FLAP_DSL}"
            "${FLAP_LAYOUT_SPEC}" "$<TARGET_OBJECTS:${FLAP_LAYOUT_PROBE_TARGET}>"
        COMMENT "Generating interpreter_${FLAP_ARCH}.S from Flap"
        COMMAND_EXPAND_LISTS
    )

    target_sources(${target} PRIVATE "${FLAP_GENERATED_S}")
    if ("${FLAP_ARCH}" STREQUAL "x86_64" AND CMAKE_ASM_COMPILER_ID MATCHES "Clang")
        # Keep conditional branches within 32-byte boundaries so Intel's
        # microcode mitigation does not prevent their blocks from using the DSB.
        set_property(SOURCE "${FLAP_GENERATED_S}" APPEND PROPERTY COMPILE_OPTIONS
            "-malign-branch-boundary=32;-malign-branch=fused,jcc")
    endif()
endmacro()
