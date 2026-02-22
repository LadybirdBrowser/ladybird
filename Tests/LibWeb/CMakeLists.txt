set(TEST_SOURCES
    TestAbstractOperations.cpp
    TestCSSIDSpeed.cpp
    TestContentFilter.cpp
    TestControlMessageQueue.cpp
    TestCSSInheritedProperty.cpp
    TestCSSPixels.cpp
    TestCSSSyntaxParser.cpp
    TestCSSTokenStream.cpp
    TestFetchURL.cpp
    TestHTMLTokenizer.cpp
    TestMicrosyntax.cpp
    TestMimeSniff.cpp
    TestNumbers.cpp
    TestStrings.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWeb LIBS LibWeb)
endforeach()

target_link_libraries(TestAbstractOperations PRIVATE LibGC LibJS)
target_link_libraries(TestContentFilter PRIVATE LibURL)
target_link_libraries(TestFetchURL PRIVATE LibURL)

add_subdirectory("test-web")
