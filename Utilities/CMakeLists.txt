ladybird_utility(js SOURCES js.cpp LIBS LibJSStandalone LibMain)
if (NOT WIN32 AND NOT ANDROID)
    target_link_libraries(js PRIVATE PkgConfig::LIBEDIT)
endif()
ladybird_utility(wasm SOURCES wasm.cpp LIBS LibFileSystem LibWasm LibMain LibJSStandalone LibCrypto LibGC)

ladybird_utility(xml SOURCES xml.cpp LIBS LibFileSystem LibMain LibXML LibURL)
ladybird_utility(dns SOURCES dns.cpp LIBS LibDNS LibMain LibTLS LibCrypto)

if (ENABLE_GUI_TARGETS)
    ladybird_utility(image SOURCES image.cpp LIBS LibGfx LibImageDecoders LibMain)
endif()

# FIXME: Increase support for building targets on Windows
if (WIN32)
    return()
endif()

ladybird_utility(test262-runner SOURCES test262-runner.cpp LIBS LibJSStandalone LibMain)
ladybird_utility(dump-html-tokens SOURCES dump-html-tokens.cpp LIBS LibMain LibWeb)
ladybird_utility(dump-html-tree SOURCES dump-html-tree.cpp LIBS LibMain LibWeb LibJS LibGC LibGfx LibIPC LibURL)

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

if (ASSERT_FAIL_HAS_INT AND TARGET test262-runner)
    target_compile_definitions(test262-runner PRIVATE ASSERT_FAIL_HAS_INT)
endif()

# Determine the Git commit hash (fallback to 'unknown' if not a Git repo)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if (NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()

# Write it to the build directory
set(GIT_COMMIT_FILE "${CMAKE_BINARY_DIR}/COMMIT")
file(WRITE "${GIT_COMMIT_FILE}" "${GIT_COMMIT_HASH}\n")

if (NOT CMAKE_SKIP_INSTALL_RULES)
    if (TARGET js)
        install(TARGETS js COMPONENT js)
    endif()
    install(FILES "${GIT_COMMIT_FILE}" COMPONENT js DESTINATION .)

    install(TARGETS wasm COMPONENT wasm)
    install(FILES "${GIT_COMMIT_FILE}" COMPONENT wasm DESTINATION .)
endif()
