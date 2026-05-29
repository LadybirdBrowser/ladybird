add_library(LibTestMain OBJECT TestMain.cpp AssertionHandler.cpp)
target_link_libraries(LibTestMain PUBLIC GenericClangPlugin)

add_library(JavaScriptTestRunnerMain OBJECT JavaScriptTestRunnerMain.cpp)
target_link_libraries(JavaScriptTestRunnerMain PRIVATE LibJS LibGC)

set(SOURCES
    TestSuite.cpp
)

add_library(LibTest ${SOURCES})
ladybird_generate_export_header(LibTest test)
target_link_libraries(LibTest PRIVATE AK LibCore LibFileSystem)
set_target_properties(LibTest PROPERTIES OUTPUT_NAME lagom-test)

if (LADYBIRD_ENABLE_PCH)
    set(DUMMY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/dummy_test_pch.cpp")
    file(WRITE ${DUMMY_SOURCE} "// Dummy file to force PCH compilation\n")
    add_library(LibTestPchTarget OBJECT ${DUMMY_SOURCE})
    target_link_libraries(LibTestPchTarget PRIVATE AK LibCore LibFileSystem LibTest)
    target_precompile_headers(LibTestPchTarget PRIVATE ${LADYBIRD_SOURCE_DIR}/AK/PCH.h TestSuite.h TestCase.h)
endif()
