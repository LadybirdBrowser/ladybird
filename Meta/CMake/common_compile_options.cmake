# Flags shared by Lagom (including Ladybird) and Serenity.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_COLOR_DIAGNOSTICS ON)

macro(add_cxx_compile_options)
  set(args "")
  foreach(arg ${ARGN})
    string(APPEND args ${arg}$<SEMICOLON>)
  endforeach()
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,ASM>:${args}>) 
endmacro()

macro(add_cxx_link_options)
  set(args "")
  foreach(arg ${ARGN})
    string(APPEND args ${arg}$<SEMICOLON>)
  endforeach()
  add_link_options($<$<LINK_LANGUAGE:C,CXX>:${args}>) 
endmacro()

if (MSVC)
    add_cxx_compile_options(/W4)
    # do not warn about unused function
    add_cxx_compile_options(/wd4505)
    # disable exceptions
    add_cxx_compile_options(/EHsc)
    # disable floating-point expression contraction
    add_cxx_compile_options(/fp:precise)
else()
    add_cxx_compile_options(-Wall -Wextra)
    add_cxx_compile_options(-fno-exceptions)
    add_cxx_compile_options(-ffp-contract=off)
endif()

add_cxx_compile_options(-Wcast-qual)
add_cxx_compile_options(-Wformat=2)
add_cxx_compile_options(-Wimplicit-fallthrough)
add_cxx_compile_options(-Wmissing-declarations)
add_cxx_compile_options(-Wsuggest-override)

add_cxx_compile_options(-Wno-invalid-offsetof)
add_cxx_compile_options(-Wno-unknown-warning-option)
add_cxx_compile_options(-Wno-unused-command-line-argument)


if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "18")
    add_cxx_compile_options(-Wpadded-bitfield)
endif()

if (NOT CMAKE_HOST_SYSTEM_NAME MATCHES SerenityOS)
    # FIXME: Something makes this go crazy and flag unused variables that aren't flagged as such when building with the toolchain.
    #        Disable -Werror for now.
    add_cxx_compile_options(-Werror)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_SIMULATE_ID  MATCHES "MSVC")
    # Clang's default constexpr-steps limit is 1048576(2^20), GCC doesn't have one
    add_cxx_compile_options(-fconstexpr-steps=16777216)

    add_cxx_compile_options(-Wmissing-prototypes)

    add_cxx_compile_options(-Wno-implicit-const-int-float-conversion)
    add_cxx_compile_options(-Wno-user-defined-literals)
    add_cxx_compile_options(-Wno-vla-cxx-extension)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Only ignore expansion-to-defined for g++, clang's implementation doesn't complain about function-like macros
    add_cxx_compile_options(-Wno-expansion-to-defined)
    add_cxx_compile_options(-Wno-literal-suffix)

    # FIXME: This warning seems useful but has too many false positives with GCC 13.
    add_cxx_compile_options(-Wno-dangling-reference)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang$" AND CMAKE_CXX_SIMULATE_ID MATCHES "MSVC")
    add_cxx_compile_options(-Wno-reserved-identifier)
    add_cxx_compile_options(-Wno-user-defined-literals)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)

    # TODO: this seems wrong, but we use this kind of code too much
    # add_cxx_compile_options(-Wno-unsafe-buffer-usage)
endif()

if (UNIX AND NOT APPLE AND NOT ENABLE_FUZZERS)
    add_cxx_compile_options(-fno-semantic-interposition)
    add_cxx_compile_options(-fvisibility-inlines-hidden)
endif()

if (NOT WIN32)
    add_cxx_compile_options(-fstack-protector-strong)
    add_cxx_link_options(-fstack-protector-strong)
endif()

add_cxx_compile_options(-fstrict-flex-arrays=2)
