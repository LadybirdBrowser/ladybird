set(SOURCES
    Assertions.cpp
    Base64.cpp
    ByteString.cpp
    ByteStringImpl.cpp
    CircularBuffer.cpp
    ConstrainedStream.cpp
    CountingStream.cpp
    Error.cpp
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
    StringConversions.cpp
    StringUtils.cpp
    StringView.cpp
    Time.cpp
    Utf16FlyString.cpp
    Utf16String.cpp
    Utf16StringData.cpp
    Utf16View.cpp
    Utf32View.cpp
    Utf8View.cpp
    kmalloc.cpp
)

if (WIN32)
    list(APPEND SOURCES
        LexicalPathWindows.cpp
        DemangleWindows.cpp
    )
else()
    list(APPEND SOURCES LexicalPath.cpp)
endif()

ladybird_lib(AK ak)

find_package(cpptrace CONFIG)
find_package(Backtrace)

if(cpptrace_FOUND AND LADYBIRD_ENABLE_CPPTRACE)
    target_link_libraries(AK PRIVATE cpptrace::cpptrace)
    target_compile_definitions(AK PRIVATE AK_HAS_CPPTRACE=1)
elseif(Backtrace_FOUND)
    if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.30)
        target_link_libraries(AK PRIVATE Backtrace::Backtrace)
    else()
        target_include_directories(AK PRIVATE ${Backtrace_INCLUDE_DIRS})
        target_link_libraries(AK PRIVATE ${Backtrace_LIBRARIES})
    endif()
else()
    message(WARNING "Cpptrace and Backtrace.h not found. Stack traces will not be available.")
endif()
configure_file(Backtrace.h.in Backtrace.h @ONLY)

find_package(simdutf REQUIRED)
target_link_libraries(AK PRIVATE simdutf::simdutf)

find_package(FastFloat CONFIG REQUIRED)
target_link_libraries(AK PRIVATE FastFloat::fast_float)

find_package(fmt CONFIG REQUIRED)
target_link_libraries(AK PRIVATE fmt::fmt)

# FIXME: Make this generic for all imported shared library dependencies and apply globally
if (BUILD_SHARED_LIBS AND NOT CMAKE_SKIP_INSTALL_RULES AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
    install(IMPORTED_RUNTIME_ARTIFACTS simdutf::simdutf
            LIBRARY COMPONENT Lagom_Runtime NAMELINK_COMPONENT Lagom_Development
            FRAMEWORK COMPONENT Lagom_Runtime
    )
endif()

if (WIN32)
    # FIXME: Windows on ARM
    target_link_libraries(AK PRIVATE clang_rt.builtins-x86_64.lib)
    target_link_libraries(AK PRIVATE Bcrypt.lib)
    target_link_libraries(AK PRIVATE delayimp.lib)
    set(DELAYLOAD_DBGHLP_VISIBILITY PRIVATE)
    if (NOT BUILD_SHARED_LIBS)
        set(DELAYLOAD_DBGHLP_VISIBILITY INTERFACE)
    endif()
    target_link_options(AK ${DELAYLOAD_DBGHLP_VISIBILITY} /DELAYLOAD:dbghelp.dll)
    # FIXME: Add support for building LibUnicode in sanitize
    #   lld-link: error: /failifmismatch: mismatch detected for 'annotate_string':
    #   >>> lagom-ak.lib(Utf16View.cpp.obj) has value 1
    #   >>> lagom-unicode.lib(IDNA.cpp.obj) has value 0
    if (ENABLE_ADDRESS_SANITIZER)
        target_compile_options(AK PRIVATE -fno-sanitize=address)
    endif()
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
