set(SOURCES
    Assertions.cpp
    Base64.cpp
    CircularBuffer.cpp
    ConstrainedStream.cpp
    CountingStream.cpp
    DOSPackedTime.cpp
    DeprecatedFlyString.cpp
    ByteString.cpp
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
    SipHash.cpp
    StackInfo.cpp
    Stream.cpp
    String.cpp
    StringBase.cpp
    StringBuilder.cpp
    StringFloatingPointConversions.cpp
    StringImpl.cpp
    StringUtils.cpp
    StringView.cpp
    Time.cpp
    Utf16View.cpp
    Utf32View.cpp
    Utf8View.cpp
    kmalloc.cpp
)

if (WIN32)
    list(APPEND SOURCES LexicalPathWindows.cpp)
else()
    list(APPEND SOURCES LexicalPath.cpp)
endif()

serenity_lib(AK ak)

serenity_install_headers(AK)
serenity_install_sources(AK)

find_package(Backtrace)
configure_file(Backtrace.h.in Backtrace.h @ONLY)

if (Backtrace_FOUND)
    if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.30)
        target_link_libraries(AK PRIVATE Backtrace::Backtrace)
    else()
        target_include_directories(AK PRIVATE ${Backtrace_INCLUDE_DIRS})
        target_link_libraries(AK PRIVATE ${Backtrace_LIBRARIES})
    endif()
else()
    message(WARNING "Backtrace not found, stack traces will be unavailable")
endif()

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
endif()
