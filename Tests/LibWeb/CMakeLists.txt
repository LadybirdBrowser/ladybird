set(TEST_SOURCES
    TestCSSIDSpeed.cpp
    TestContentBlocker.cpp
    TestControlMessageQueue.cpp
    TestCSSInheritedProperty.cpp
    TestCSSPixels.cpp
    TestCSSStyleSheetInvalidation.cpp
    TestCSSSyntaxParser.cpp
    TestCSSTokenizer.cpp
    TestCSSTokenStream.cpp
    TestFetchURL.cpp
    TestHTMLTokenizer.cpp
    TestMicrosyntax.cpp
    TestMimeSniff.cpp
    TestNumbers.cpp
    TestRefCountedTreeNode.cpp
    TestSecureContexts.cpp
    TestSourceHighlighter.cpp
    TestStrings.cpp
)

ladybird_test_suite(TestWeb LibWeb SOURCES ${TEST_SOURCES} LIBS LibWeb LibURL LibSync LibWebView)

ladybird_utility(css-tokenizer SOURCES css-tokenizer.cpp LIBS LibFileSystem LibMain LibWeb)

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
