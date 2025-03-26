# Flags shared by Lagom (including Ladybird) and Serenity.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_COLOR_DIAGNOSTICS ON)

macro(add_cxx_compile_options)
  set(args "")
  foreach(arg ${ARGN})
    string(APPEND args ${arg}$<SEMICOLON>)
    add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc ${arg}>")
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

# FIXME: Rework these flags to remove the suspicious ones.
if (WIN32)
  add_compile_options(-Wno-unknown-attributes) # [[no_unique_address]] is broken in MSVC ABI until next ABI break
  add_compile_options(-Wno-reinterpret-base-class)
  add_compile_options(-Wno-microsoft-unqualified-friend) # MSVC doesn't support unqualified friends
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS) # _s replacements not desired (or implemented on any other platform other than VxWorks)
  add_compile_definitions(_CRT_NONSTDC_NO_WARNINGS) # POSIX names are just fine, thanks
  add_compile_definitions(_USE_MATH_DEFINES)
  add_compile_definitions(NOMINMAX)
  add_compile_definitions(WIN32_LEAN_AND_MEAN)
  add_compile_definitions(NAME_MAX=255)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
  add_compile_options(-Wno-deprecated-declarations)
endif()

if (MSVC)
    add_cxx_compile_options(/W4)
    # disable exceptions
    add_cxx_compile_options(/EHs-)
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
add_cxx_compile_options(-Wlogical-op)
add_cxx_compile_options(-Wmissing-declarations)
add_cxx_compile_options(-Wmissing-field-initializers)
add_cxx_compile_options(-Wsuggest-override)

add_cxx_compile_options(-Wno-invalid-offsetof)
add_cxx_compile_options(-Wno-unknown-warning-option)
add_cxx_compile_options(-Wno-unused-command-line-argument)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "18")
    add_cxx_compile_options(-Wpadded-bitfield)
endif()

add_cxx_compile_options(-Werror)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_SIMULATE_ID  MATCHES "MSVC")
    # Clang's default constexpr-steps limit is 1048576(2^20), GCC doesn't have one
    add_cxx_compile_options(-fconstexpr-steps=16777216)

    add_cxx_compile_options(-Wmissing-prototypes)

    add_cxx_compile_options(-Wno-implicit-const-int-float-conversion)
    add_cxx_compile_options(-Wno-user-defined-literals)
    add_cxx_compile_options(-Wno-unqualified-std-cast-call)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Only ignore expansion-to-defined for g++, clang's implementation doesn't complain about function-like macros
    add_cxx_compile_options(-Wno-expansion-to-defined)
    add_cxx_compile_options(-Wno-literal-suffix)
    add_cxx_compile_options(-Wno-unqualified-std-cast-call)
    add_cxx_compile_options(-Wvla)

    # FIXME: This warning seems useful but has too many false positives with GCC 13.
    add_cxx_compile_options(-Wno-dangling-reference)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang$" AND CMAKE_CXX_SIMULATE_ID MATCHES "MSVC")
    add_cxx_compile_options(-Wno-implicit-const-int-float-conversion)
    add_cxx_compile_options(-Wno-reserved-identifier)
    add_cxx_compile_options(-Wno-user-defined-literals)
    add_cxx_compile_options(-Wno-unqualified-std-cast-call)
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

if (NOT MSVC)
    add_cxx_compile_options(-fstrict-flex-arrays=2)
endif()
