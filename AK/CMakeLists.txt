set(AK_SOURCES
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
    FuzzyMatch.cpp
    GenericLexer.cpp
    Hex.cpp
    JsonObject.cpp
    JsonParser.cpp
    JsonPath.cpp
    JsonValue.cpp
    LexicalPath.cpp
    MemoryStream.cpp
    NumberFormat.cpp
    OptionParser.cpp
    Random.cpp
    SipHash.cpp
    Slugify.cpp
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
    UUID.cpp
    Utf16View.cpp
    Utf32View.cpp
    Utf8View.cpp
    kmalloc.cpp
)
# AK sources are included from many different places
list(TRANSFORM AK_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

set(AK_SOURCES ${AK_SOURCES} PARENT_SCOPE)

serenity_install_headers(AK)
serenity_install_sources(AK)
