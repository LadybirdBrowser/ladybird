add_subdirectory("test-web")

if (BUILD_TESTING AND NOT WIN32)
    add_test(
        NAME TestWebDriverHTTPErrorResponses
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test-webdriver-http-error-responses.py $<TARGET_FILE:WebDriver>
    )
    set_tests_properties(TestWebDriverHTTPErrorResponses PROPERTIES TIMEOUT 120)
endif()

if (BUILD_BROWSER_TESTS_ONLY)
    return()
endif()

ladybird_test_suite(LibWeb)

set(TEST_SOURCES
    BenchmarkDisplayListRecording.cpp
    TestAudioWorkletPipe.cpp
    TestBlobBytes.cpp
    TestContentBlocker.cpp
    TestControlMessageQueue.cpp
    TestCSSDecodeBytes.cpp
    TestCSSIDSpeed.cpp
    TestCSSInheritedProperty.cpp
    TestCSSPropertyList.cpp
    TestDownloadReader.cpp
    TestDump.cpp
    TestEventLoop.cpp
    TestFaviconDecoding.cpp
    TestFetchAlgorithms.cpp
    TestFetchBodyDeliveryGate.cpp
    TestFetchBodyStream.cpp
    TestFetchByteChannel.cpp
    TestFetchController.cpp
    TestFetchControllerRegistry.cpp
    TestFetchEngineMessages.cpp
    TestFetchExecutor.cpp
    TestFetchResponse.cpp
    TestFetchTaskDestination.cpp
    TestFetchURL.cpp
    TestHighResolutionTime.cpp
    TestHTMLTokenizer.cpp
    TestImageData.cpp
    TestInitialLoadTracker.cpp
    TestInlineStyle.cpp
    TestLoaderConfig.cpp
    TestMicrosyntax.cpp
    TestMimeSniff.cpp
    TestNumbers.cpp
    TestSecureContexts.cpp
    TestSessionHistoryEntry.cpp
    TestSharedCompiledStyleSheet.cpp
    TestSimpleRealm.cpp
    TestSourceHighlighter.cpp
    TestStorageAreaMirror.cpp
    TestStructuredSerializeCorpus.cpp
    TestStructuredSerializeDurability.cpp
    TestStructuredSerializeFormat.cpp
    TestStructuredSerializeHardening.cpp
    TestStructuredSerializeSharedArrayBuffer.cpp
    TestStyleEngineBridge.cpp
    TestWebIDLBuffers.cpp
    TestWindowProxyWorld.cpp
    TestWorldCallback.cpp
    TestWrapperWorld.cpp
)

if (NOT WIN32)
    list(APPEND TEST_SOURCES
        # These call Rust FFI symbols directly, which the lagom-web DLL does not export.
        TestCSSSyntaxParser.cpp
        TestLengthAbsolutizeParity.cpp
        TestStylePropertyMetadataParity.cpp
        TestStyleStructRef.cpp
        TestStyleValueEquality.cpp
    )
endif()

target_sources(TestLibWeb PRIVATE
    ${TEST_SOURCES}
    ${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/DOM/IDLEventListener.cpp
    ${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/WebIDL/CallbackType.cpp
)
target_link_libraries(TestLibWeb PRIVATE LibCompositing LibCrypto LibGC LibGfx LibHTTP LibIPC LibJS LibRequests LibThreading LibURL LibWeb LibWebCommon LibWebView)

# The check reads the symbols of the build's objects with nm, which can't see into an LTO build's bitcode objects and
# isn't the toolchain's symbol reader on Windows.
if (BUILD_TESTING AND NOT WIN32 AND NOT CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    add_test(
        NAME TestDeadRustExports
        COMMAND ${Python3_EXECUTABLE} ${LADYBIRD_SOURCE_DIR}/Meta/check-dead-rust-exports.py
            --nm ${CMAKE_NM}
            --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
            --source-root ${LADYBIRD_SOURCE_DIR}
            $<TARGET_FILE:libweb_bindings_rust>
            $<TARGET_FILE:libweb_content_blocker_rust>
    )
endif()


add_subdirectory("AccessibilityBridge")
add_subdirectory("AccessibilityBridgeMac")
