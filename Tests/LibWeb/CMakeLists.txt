set(TEST_SOURCES
    TestCSSIDSpeed.cpp
    TestCSSPixels.cpp
    TestFetchInfrastructure.cpp
    TestFetchURL.cpp
    TestHTMLTokenizer.cpp
    TestMicrosyntax.cpp
    TestMimeSniff.cpp
    TestNumbers.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibWeb LIBS LibWeb)
endforeach()

target_link_libraries(TestFetchURL PRIVATE LibURL)

if (ENABLE_SWIFT)
    add_executable(TestLibWebSwiftBindings TestLibWebSwiftBindings.swift)
    target_link_libraries(TestLibWebSwiftBindings PRIVATE AK LibWeb)
    target_compile_options(TestLibWebSwiftBindings PRIVATE -parse-as-library)
    add_test(NAME TestLibWebSwiftBindings COMMAND TestLibWebSwiftBindings)
endif()
