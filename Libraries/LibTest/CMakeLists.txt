serenity_install_sources("Libraries/LibTest")

set(SOURCES
    TestSuite.cpp
    CrashTest.cpp
)

serenity_lib(LibTest test)

add_library(LibTestMain OBJECT TestMain.cpp)
add_library(JavaScriptTestRunnerMain OBJECT JavaScriptTestRunnerMain.cpp)

if (SERENITYOS)
    add_dependencies(LibTestMain install_libc_headers)
    add_dependencies(JavaScriptTestRunnerMain install_libc_headers)
endif()
