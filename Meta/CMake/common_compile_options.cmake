# Flags shared by Lagom (including Ladybird) and Serenity.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_COLOR_DIAGNOSTICS ON)

macro(add_cxx_compile_options)
    set(args "")
    foreach(arg ${ARGN})
        string(APPEND args ${arg}$<SEMICOLON>)
        add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc ${arg}>")
    endforeach()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,ASM>:${args}>)
endmacro()

macro(add_cxx_compile_definitions)
    set(args "")
    foreach(arg ${ARGN})
        string(APPEND args ${arg}$<SEMICOLON>)
        add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc -D${arg}>")
    endforeach()
    add_compile_definitions($<$<COMPILE_LANGUAGE:C,CXX,ASM>:${args}>)
endmacro()

macro(add_cxx_link_options)
    set(args "")
    foreach(arg ${ARGN})
        string(APPEND args ${arg}$<SEMICOLON>)
    endforeach()
    add_link_options($<$<LINK_LANGUAGE:C,CXX>:${args}>)
endmacro()

macro(add_swift_compile_options)
    set(args "")
    foreach(arg ${ARGN})
        string(APPEND args ${arg}$<SEMICOLON>)
    endforeach()
    add_compile_options($<$<COMPILE_LANGUAGE:Swift>:${args}>)
endmacro()

macro(add_swift_link_options)
    set(args "")
    foreach(arg ${ARGN})
        string(APPEND args ${arg}$<SEMICOLON>)
    endforeach()
    add_link_options($<$<LINK_LANGUAGE:Swift>:${args}>)
endmacro()

include(CheckLinkerFlag)
include(CMakePushCheckState)
function(add_cxx_link_option_if_supported option)
    cmake_push_check_state()
    if (MSVC)
        set(CMAKE_REQUIRED_LINK_OPTIONS /WX)
    else()
        set(CMAKE_REQUIRED_LINK_OPTIONS -Werror)
    endif()
    check_linker_flag(CXX ${option} LAGOM_LINKER_SUPPORTS_${option})
    if (${LAGOM_LINKER_SUPPORTS_${option}})
        add_cxx_link_options(${option})
    endif()
    cmake_pop_check_state()
endfunction()

if (ENABLE_CI_BASELINE_CPU)
    # In CI, we want to target a common architecture so different runners can share ccache caches effectively.
    if (APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        add_cxx_compile_options(-mcpu=apple-m1)
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        add_cxx_compile_options(-march=armv8.2-a)
    elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_cxx_compile_options(-march=x86-64-v3)
    endif()
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
    # On RISC-V the generic -march=native is not yet supported and both gcc and clang require an explicit
    # ISA or target string. Unfortunately hardware probing is also neither easy nor reliable at the moment.
    # For the time being use the defaults for the best compatibility with existing hardware and toolchains.
    # FIXME: Remove this branch once -march=native is supported.
elseif (NOT CMAKE_CROSSCOMPILING)
    # In all other cases, compile for the native architecture of the host system.
    add_cxx_compile_options(-march=native)
endif()

add_cxx_compile_options(-Wcast-qual)
add_cxx_compile_options(-Wformat=2)
add_cxx_compile_options(-Wimplicit-fallthrough)
add_cxx_compile_options(-Wlogical-op)
add_cxx_compile_options(-Wmissing-declarations)
add_cxx_compile_options(-Wmissing-field-initializers)
add_cxx_compile_options(-Wsuggest-override)

add_cxx_compile_options(-Wno-invalid-offsetof)
add_cxx_compile_options(-Wno-unknown-warning-option)
add_cxx_compile_options(-Wno-unused-command-line-argument)

add_cxx_compile_options(-Werror)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "18")
    add_cxx_compile_options(-Wpadded-bitfield)
endif()

if (NOT MSVC)
    add_cxx_compile_options(-Wall -Wextra)
    add_cxx_compile_options(-fno-exceptions)
    add_cxx_compile_options(-ffp-contract=off)
    add_cxx_compile_options(-fstrict-flex-arrays=2)
    add_cxx_compile_options(-fstack-protector-strong)
    add_cxx_link_options(-fstack-protector-strong)
    if (UNIX AND NOT APPLE AND NOT ENABLE_FUZZERS)
        add_cxx_compile_options(-fno-semantic-interposition)
        add_cxx_compile_options(-fvisibility-inlines-hidden)
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_SIMULATE_ID  MATCHES "MSVC")
    # Clang's default constexpr-steps limit is 1048576(2^20), GCC doesn't have one
    add_cxx_compile_options(-fconstexpr-steps=16777216)

    add_cxx_compile_options(-Wmissing-prototypes)

    add_cxx_compile_options(-Wno-implicit-const-int-float-conversion)
    add_cxx_compile_options(-Wno-user-defined-literals)
    add_cxx_compile_options(-Wno-unqualified-std-cast-call)

    # Used for the #embed directive.
    # FIXME: Remove this once #embed is no longer an extension.
    add_cxx_compile_options(-Wno-c23-extensions)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Only ignore expansion-to-defined for g++, clang's implementation doesn't complain about function-like macros
    add_cxx_compile_options(-Wno-expansion-to-defined)
    add_cxx_compile_options(-Wno-literal-suffix)
    add_cxx_compile_options(-Wno-unqualified-std-cast-call)
    add_cxx_compile_options(-Wvla)

    # FIXME: These warnings trigger on Function and ByteBuffer in GCC (only when LTO is disabled...)
    #        investigate this and maybe reenable them if they're not false positives/invalid.
    add_cxx_compile_options(-Wno-array-bounds)
    add_cxx_compile_options(-Wno-stringop-overflow)

    # FIXME: This warning seems useful but has too many false positives with GCC 13.
    add_cxx_compile_options(-Wno-dangling-reference)
elseif (MSVC)
    # Warning options and defines
    add_cxx_compile_options(/W4)
    add_cxx_compile_options(-Wno-implicit-const-int-float-conversion)
    add_cxx_compile_options(-Wno-reserved-identifier)
    add_cxx_compile_options(-Wno-user-defined-literals)
    add_cxx_compile_options(-Wno-unqualified-std-cast-call)
    add_cxx_compile_options(-Wno-c23-extensions)
    add_cxx_compile_options(-Wno-microsoft-unqualified-friend) # MSVC doesn't support unqualified friends
    add_cxx_compile_definitions(_CRT_SECURE_NO_WARNINGS) # _s replacements not desired (or implemented on any other platform other than VxWorks)
    add_cxx_compile_definitions(_CRT_NONSTDC_NO_WARNINGS) # POSIX names are just fine, thanks

    # Windows API defines
    add_cxx_compile_definitions(_USE_MATH_DEFINES)
    add_cxx_compile_definitions(NOMINMAX)
    add_cxx_compile_definitions(WIN32_LEAN_AND_MEAN)
    add_cxx_compile_definitions(_WIN32_WINNT=0x0A00)
    add_cxx_compile_definitions(WINVER=0x0A00)

    # TODO: Use export macros everywhere
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

    # Compiler options
    # disable exceptions
    add_cxx_compile_options(/EHs-)
    # disable floating-point expression contraction
    add_cxx_compile_options(/fp:precise)
    # reduces object size
    add_cxx_compile_options(/Zc:inline)
    # equivalent of -fvisibility-inlines-hidden
    add_cxx_compile_options(/Zc:dllexportInlines-)
    # clang-cl has this off by default unlike other clang versions
    add_cxx_compile_options(-fstrict-aliasing)
    add_cxx_compile_options(/clang:-fstrict-flex-arrays=2)
    # create COMDATs from functions and data, enables deduplication
    add_cxx_compile_options(/Gw)
    add_cxx_compile_options(/Gy)
    # enable control flow protection
    add_cxx_compile_options(/guard:cf)

    # Linker options
    # increase stack size reserve to match Linux
    add_cxx_link_options(/STACK:0x800000)
    # enable control flow protection
    add_cxx_link_options(/GUARD:CF)
    add_cxx_link_option_if_supported(-prefetch-inputs)

    # Use ghash compressed debug info for build time and pdb size reduction
    if (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR "Debug")
        add_cxx_compile_options(-gcodeview-ghash)
        add_cxx_link_options(/DEBUG:GHASH)
    endif()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/sanitizers.cmake)
