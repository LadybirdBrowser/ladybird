set(SOURCES
    Assertions.cpp
    Base64.cpp
    ByteString.cpp
    ByteStringImpl.cpp
    CircularBuffer.cpp
    ConstrainedStream.cpp
    CountingStream.cpp
    Error.cpp
    FloatingPointStringConversions.cpp
    FlyString.cpp
    Format.cpp
    GenericLexer.cpp
    Hex.cpp
    JsonArray.cpp
    JsonObject.cpp
    JsonParser.cpp
    JsonValue.cpp
    MemoryStream.cpp
    NumberFormat.cpp
    OptionParser.cpp
    Random.cpp
    StackInfo.cpp
    Stream.cpp
    String.cpp
    StringBase.cpp
    StringBuilder.cpp
    StringFloatingPointConversions.cpp
    StringUtils.cpp
    StringView.cpp
    Time.cpp
    Utf16View.cpp
    Utf32View.cpp
    Utf8View.cpp
    kmalloc.cpp
)

if (WIN32)
    list(APPEND SOURCES LexicalPathWindows.cpp DemangleWindows.cpp)
else()
    list(APPEND SOURCES LexicalPath.cpp)
endif()

serenity_lib(AK ak)

include(stacktrace)
configure_file(Backtrace.h.in Backtrace.h @ONLY)
link_stacktrace_library(AK STD_DEFINITION AK_HAS_STD_STACKTRACE)

find_package(simdutf REQUIRED)
swizzle_target_properties_for_swift(simdutf::simdutf)
target_link_libraries(AK PRIVATE simdutf::simdutf)

# FIXME: Make this generic for all imported shared library dependencies and apply globally
if (BUILD_SHARED_LIBS AND NOT CMAKE_SKIP_INSTALL_RULES AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
    install(IMPORTED_RUNTIME_ARTIFACTS simdutf::simdutf
            LIBRARY COMPONENT Lagom_Runtime NAMELINK_COMPONENT Lagom_Development
            FRAMEWORK COMPONENT Lagom_Runtime
    )
endif()

if (ENABLE_SWIFT)
    generate_clang_module_map(AK
       GENERATED_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/Backtrace.h"
        "${CMAKE_CURRENT_BINARY_DIR}/Debug.h"
    )
    target_sources(AK PRIVATE AK+Swift.swift)
    add_swift_target_properties(AK)
endif()

if (WIN32)
    # FIXME: Windows on ARM
    target_link_libraries(AK PRIVATE clang_rt.builtins-x86_64.lib)
    target_link_libraries(AK PRIVATE Bcrypt.lib)
    target_link_libraries(AK PRIVATE delayimp.lib)
    target_link_options(AK PRIVATE /DELAYLOAD:dbghelp.dll)
elseif (APPLE)
    set(ASSERTION_HANDLER_VISIBILITY PRIVATE)
    if (NOT BUILD_SHARED_LIBS)
        set(ASSERTION_HANDLER_VISIBILITY INTERFACE)
    endif()
    target_link_options(AK ${ASSERTION_HANDLER_VISIBILITY} LINKER:-U,_ak_assertion_handler)
endif()

# Manually install AK headers
if (ENABLE_INSTALL_HEADERS)
    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMPONENT Lagom_Development
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/Debug.h
        ${CMAKE_CURRENT_BINARY_DIR}/Backtrace.h
        COMPONENT Lagom_Development
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/AK"
    )
endif()
