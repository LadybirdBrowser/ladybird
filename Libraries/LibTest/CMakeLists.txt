add_library(LibTestMain OBJECT TestMain.cpp AssertionHandler.cpp)

target_link_libraries(LibTestMain PUBLIC GenericClangPlugin)

set(SOURCES
    TestSuite.cpp
)

add_library(LibTest ${SOURCES})
lagom_generate_export_header(LibTest test)
target_link_libraries(LibTest PRIVATE AK LibCore LibFileSystem)
set_target_properties(LibTest PROPERTIES OUTPUT_NAME lagom-test)

# FIXME: Increase support for building targets on Windows
if (WIN32 AND ENABLE_WINDOWS_CI)
    return()
endif()

add_library(JavaScriptTestRunnerMain OBJECT JavaScriptTestRunnerMain.cpp)
