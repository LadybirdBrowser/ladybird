set(TEST_SOURCES
    TestSiteCompatibility.cpp
    TestContentBlocker.cpp
    TestControlMessageQueue.cpp
    TestCSSIDSpeed.cpp
    TestCSSInheritedProperty.cpp
    TestCSSPixels.cpp
    TestCSSDecodeBytes.cpp
    TestDisplayListCommandRuns.cpp
    TestDownloadReader.cpp
    TestDump.cpp
    TestEventLoop.cpp
    TestFetchResponse.cpp
    TestFetchURL.cpp
    TestHTMLTokenizer.cpp
    TestHighResolutionTime.cpp
    TestImageData.cpp
    TestLengthAbsolutizeParity.cpp
    TestMicrosyntax.cpp
    TestMimeSniff.cpp
    TestNumbers.cpp
    TestScrollState.cpp
    TestSecureContexts.cpp
    TestSessionHistoryEntry.cpp
    TestSimpleRealm.cpp
    TestSmoothScrollAnimation.cpp
    TestSourceHighlighter.cpp
    TestStrings.cpp
    TestStructuredSerializeCorpus.cpp
    TestStructuredSerializeDurability.cpp
    TestStructuredSerializeFormat.cpp
    TestStructuredSerializeHardening.cpp
    TestStyleEngineBridge.cpp
    TestStylePropertyMetadataParity.cpp
    TestStyleStructRef.cpp
    TestTextureUpload.cpp
    TestTrackBufferDemuxer.cpp
    TestVisualAnimation.cpp
    TestWebGLSpanWithStorage.cpp
    TestWebIDLBuffers.cpp
    TestWindowProxyWorld.cpp
    TestWorldCallback.cpp
    TestWrapperWorld.cpp
)

if (NOT WIN32)
    list(APPEND TEST_SOURCES
        # These call Rust FFI symbols directly, which the lagom-web DLL does not export.
        TestCSSSyntaxParser.cpp
        TestStyleValueEquality.cpp
    )
endif()

foreach(source IN LISTS TEST_SOURCES)
    set(test_libraries LibWeb)
    if (source STREQUAL "TestSimpleRealm.cpp")
        list(APPEND test_libraries LibGC LibJS)
    endif()
    ladybird_test("${source}" LibWeb LIBS ${test_libraries})
endforeach()


target_link_libraries(TestContentBlocker PRIVATE LibURL)
target_link_libraries(TestSiteCompatibility PRIVATE LibURL)
target_link_libraries(TestControlMessageQueue PRIVATE LibSync)
target_link_libraries(TestDisplayListCommandRuns PRIVATE LibGfx)
target_link_libraries(TestDownloadReader PRIVATE LibGC LibJS LibURL)
target_link_libraries(TestFetchResponse PRIVATE LibGC LibHTTP LibJS LibRequests LibURL)
target_link_libraries(TestFetchURL PRIVATE LibURL)
target_link_libraries(TestImageData PRIVATE LibGC LibJS)
target_link_libraries(TestWebIDLBuffers PRIVATE LibGC LibJS)
target_link_libraries(TestWebGLSpanWithStorage PRIVATE LibGC LibJS)
target_link_libraries(TestScrollState PRIVATE LibGfx LibIPC)
target_link_libraries(TestStructuredSerializeCorpus PRIVATE LibGC LibJS LibCrypto LibGfx LibIPC)
target_link_libraries(TestStructuredSerializeDurability PRIVATE LibGC LibJS LibCrypto LibGfx LibIPC)
target_link_libraries(TestStructuredSerializeFormat PRIVATE LibGC LibJS LibCrypto LibGfx LibIPC)
target_link_libraries(TestStructuredSerializeHardening PRIVATE LibGC LibJS LibCrypto LibGfx LibIPC)
target_link_libraries(TestSecureContexts PRIVATE LibURL)
target_link_libraries(TestSessionHistoryEntry PRIVATE LibJS LibURL)
target_link_libraries(TestSourceHighlighter PRIVATE LibURL LibWebView)
target_link_libraries(TestWrapperWorld PRIVATE LibGC LibJS LibURL)
target_sources(TestWorldCallback PRIVATE
    ${ladybird_SOURCE_DIR}/Libraries/LibWeb/DOM/IDLEventListener.cpp
    ${ladybird_SOURCE_DIR}/Libraries/LibWeb/WebIDL/CallbackType.cpp
)
target_link_libraries(TestWorldCallback PRIVATE LibGC LibJS LibWeb)
target_sources(TestWindowProxyWorld PRIVATE
    ${ladybird_SOURCE_DIR}/Libraries/LibWeb/DOM/IDLEventListener.cpp
    ${ladybird_SOURCE_DIR}/Libraries/LibWeb/WebIDL/CallbackType.cpp
)
target_link_libraries(TestWindowProxyWorld PRIVATE LibGC LibJS LibWeb)

add_subdirectory("test-web")
