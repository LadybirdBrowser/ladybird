set(TEST_SOURCES
    TestCSSIDSpeed.cpp
    TestContentFilter.cpp
    TestControlMessageQueue.cpp
    TestCSSInheritedProperty.cpp
    TestCSSPixels.cpp
    TestCSSStyleSheetInvalidation.cpp
    TestCSSSyntaxParser.cpp
    TestCSSTokenStream.cpp
    TestFetchURL.cpp
    TestHTMLTokenizer.cpp
    TestMicrosyntax.cpp
    TestMimeSniff.cpp
    TestNumbers.cpp
    TestSourceHighlighter.cpp
    TestStrings.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWeb LIBS LibWeb)
endforeach()

lagom_utility(css-tokenizer SOURCES css-tokenizer.cpp LIBS LibFileSystem LibMain LibWeb)

target_link_libraries(TestContentFilter PRIVATE LibURL)
target_link_libraries(TestFetchURL PRIVATE LibURL)
target_link_libraries(TestSourceHighlighter PRIVATE LibURL LibWebView)

if (NOT WIN32)
    add_custom_target(test-css-tokenizer ALL DEPENDS css-tokenizer "${CMAKE_BINARY_DIR}/bin/test-css-tokenizer")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/bin/test-css-tokenizer"
        COMMAND "${CMAKE_COMMAND}" -E copy "${ladybird_SOURCE_DIR}/Tests/LibWeb/test-css-tokenizer.py" "${CMAKE_BINARY_DIR}/bin/test-css-tokenizer"
        DEPENDS "${ladybird_SOURCE_DIR}/Tests/LibWeb/test-css-tokenizer.py"
    )
    add_test(NAME test-css-tokenizer COMMAND "${CMAKE_BINARY_DIR}/bin/test-css-tokenizer")
    set_tests_properties(test-css-tokenizer PROPERTIES ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
endif()

add_subdirectory("test-web")
