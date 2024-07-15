set(SOURCES
    Assertions.cpp
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
    JsonObject.cpp
    JsonParser.cpp
    JsonValue.cpp
    LexicalPath.cpp
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

if (NOT LAGOM_TOOLS_ONLY)
    list(APPEND SOURCES Base64.cpp)
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

if (NOT LAGOM_TOOLS_ONLY)
    find_package(simdutf REQUIRED)
    target_link_libraries(AK PRIVATE simdutf::simdutf)
endif()
