set(TEST_SOURCES
    TestCSSIDSpeed.cpp
    TestAccumulatedVisualContext.cpp
    TestContentBlocker.cpp
    TestControlMessageQueue.cpp
    TestCSSInheritedProperty.cpp
    TestCSSPixels.cpp
    TestCSSStyleSheetInvalidation.cpp
    TestCSSSyntaxParser.cpp
    TestCSSTokenizer.cpp
    TestCSSTokenStream.cpp
    TestFetchResponse.cpp
    TestFetchURL.cpp
    TestHTMLTokenizer.cpp
    TestImageData.cpp
    TestMicrosyntax.cpp
    TestMimeSniff.cpp
    TestNumbers.cpp
    TestPage.cpp
    TestRefCountedTreeNode.cpp
    TestSecureContexts.cpp
    TestSessionHistoryEntry.cpp
    TestSourceHighlighter.cpp
    TestStrings.cpp
    TestWebIDLBuffers.cpp
    TestWebGLSpanWithStorage.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWeb LIBS LibWeb)
endforeach()

ladybird_utility(css-tokenizer SOURCES css-tokenizer.cpp LIBS LibFileSystem LibMain LibWeb)

target_link_libraries(TestContentBlocker PRIVATE LibURL)
target_link_libraries(TestControlMessageQueue PRIVATE LibSync)
target_link_libraries(TestFetchResponse PRIVATE LibGC LibHTTP LibJS)
target_link_libraries(TestFetchURL PRIVATE LibURL)
target_link_libraries(TestAccumulatedVisualContext PRIVATE LibGfx)
target_link_libraries(TestImageData PRIVATE LibGC LibJS)
target_link_libraries(TestPage PRIVATE LibGC LibJS)
target_link_libraries(TestSecureContexts PRIVATE LibURL)
target_link_libraries(TestSessionHistoryEntry PRIVATE LibJS LibURL)
target_link_libraries(TestSourceHighlighter PRIVATE LibURL LibWebView)
target_link_libraries(TestWebIDLBuffers PRIVATE LibGC LibJS)
target_link_libraries(TestWebGLSpanWithStorage PRIVATE LibGC LibJS)

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
