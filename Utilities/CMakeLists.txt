# FIXME: Increase support for building targets on Windows
if (WIN32 AND ENABLE_WINDOWS_CI)
    return()
endif()

lagom_utility(abench SOURCES abench.cpp LIBS LibMain LibFileSystem LibMedia)
lagom_utility(dns SOURCES dns.cpp LIBS LibDNS LibMain LibTLS LibCrypto)

if (ENABLE_GUI_TARGETS)
    lagom_utility(animation SOURCES animation.cpp LIBS LibGfx LibMain)
    lagom_utility(image SOURCES image.cpp LIBS LibGfx LibMain)
endif()

lagom_utility(js SOURCES js.cpp LIBS LibCrypto LibJS LibLine LibUnicode LibMain LibTextCodec LibGC Threads::Threads)
lagom_utility(test262-runner SOURCES test262-runner.cpp LIBS LibJS LibFileSystem LibGC)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckCSourceCompiles)
    # Check for musl's declaration of __assert_fail
    check_c_source_compiles("
    #include <assert.h>
    __attribute__((__noreturn__)) void __assert_fail(char const* assertion, char const* file, int line, char const* function) {}
    int main() {}
    "
    ASSERT_FAIL_HAS_INT
    )
endif()

if (ASSERT_FAIL_HAS_INT OR EMSCRIPTEN)
    target_compile_definitions(test262-runner PRIVATE ASSERT_FAIL_HAS_INT)
endif()

lagom_utility(wasm SOURCES wasm.cpp LIBS LibFileSystem LibWasm LibLine LibMain)
lagom_utility(xml SOURCES xml.cpp LIBS LibFileSystem LibMain LibXML LibURL)

if (NOT CMAKE_SKIP_INSTALL_RULES)
    install(TARGETS js COMPONENT js)

    set(CPACK_GENERATOR "TGZ")
    set(CPACK_STRIP_FILES TRUE)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_ALL js)
    if (APPLE)
        if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
            set(CPACK_SYSTEM_NAME "macOS-universal2")
        else()
            set(CPACK_SYSTEM_NAME "macOS-${CMAKE_SYSTEM_PROCESSOR}")
        endif()
    else()
        set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(CPACK_ARCHIVE_JS_FILE_NAME "ladybird-js-${CPACK_SYSTEM_NAME}")
    set(CPACK_PACKAGE_FILE_NAME "ladybird-js-${CPACK_SYSTEM_NAME}")
    include(CPack)
endif()
