set(SOURCES
    AccessibilityTreeManager.cpp
    Application.cpp
    ApplyHistoryStep.cpp
    Autocomplete.cpp
    AutocompleteMuxer.cpp
    AutocompleteRanker.cpp
    AutocompleteService.cpp
    BlobURLStore.cpp
    BrowsingSession.cpp
    BookmarkStore.cpp
    CanonicalBrowsingContext.cpp
    CanonicalBrowsingContextGroup.cpp
    CanonicalDocument.cpp
    CanonicalEnvironmentSettingsObject.cpp
    CanonicalNavigable.cpp
    CanonicalSessionHistoryEntry.cpp
    CanonicalTraversable.cpp
    CanonicalWindow.cpp
    BrowserProcess.cpp
    CompositorClient.cpp
    CookieJar.cpp
    CrashReport.cpp
    CrashReportDiagnostics.cpp
    CrashReportReview.cpp
    CrashReportStore.cpp
    CrashReportSubmission.cpp
    DownloadPresentation.cpp
    DownloadSegmentation.cpp
    DownloadStore.cpp
    ExternalURLHandler.cpp
    FaviconStore.cpp
    FileDownloader.cpp
    FontService.cpp
    FontServiceHost.cpp
    GamepadManager.cpp
    HeadlessWebView.cpp
    HelperProcess.cpp
    HistoryDebug.cpp
    HistoryStore.cpp
    HSTSStore.cpp
    Menu.cpp
    NavigationLoader.cpp
    Omnibox.cpp
    OmniboxEngagement.cpp
    Process.cpp
    ProcessManager.cpp
    TabPerformanceStats.cpp
    TabPerformanceMonitor.cpp
    ProcessMonitor.cpp
    Profile.cpp
    RequestServerManager.cpp
    RequestServerSiteBindings.cpp
    SearchEngine.cpp
    SessionHistory.cpp
    SessionHistorySnapshotStorage.cpp
    SessionHistoryTraversalQueue.cpp
    SessionStore.cpp
    Settings.cpp
    SiteCompatibility.cpp
    StorageJar.cpp
    URL.cpp
    UserAgent.cpp
    Utilities.cpp
    ViewImplementation.cpp
    WebContentClient.cpp
    WebContentPage.cpp
    WebContentTestClient.cpp
    WebDriverBrowserConnection.cpp
    WebUI.cpp
    WebUI/BookmarksUI.cpp
    WebUI/DownloadsUI.cpp
    WebUI/HistoryUI.cpp
    WebUI/SettingsUI.cpp
    WebUI/VersionUI.cpp
    WebWorkerClient.cpp
    WorkerProcessManager.cpp
)

if (APPLE)
    list(APPEND SOURCES
        CrashReportPOSIX.cpp
        LaunchURLsMacOS.cpp
        PlatformColorsMacOS.cpp
        ProcessReaper.cpp
    )
elseif (LINUX)
    list(APPEND SOURCES CrashReportPOSIX.cpp)
endif()

set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

compile_ipc(UIProcessServer.ipc UIProcessServerEndpoint.h)
compile_ipc(UIProcessClient.ipc UIProcessClientEndpoint.h)

set(GENERATED_SOURCES
    ${GENERATED_SOURCES}
    ../../Services/RequestServer/RequestClientEndpoint.h
    ../../Services/RequestServer/RequestServerEndpoint.h
    UIProcessClientEndpoint.h
    UIProcessServerEndpoint.h
)

set(build_information_header "${CMAKE_CURRENT_BINARY_DIR}/BuildInformation.h")
set(build_information_stamp "${CMAKE_CURRENT_BINARY_DIR}/BuildInformation.stamp")
set(build_information_depfile "${CMAKE_CURRENT_BINARY_DIR}/BuildInformation.d")
set(build_information_generator "${LADYBIRD_SOURCE_DIR}/Meta/Generators/generate-build-information.py")

add_custom_command(
    COMMAND
        "${Python3_EXECUTABLE}" "${build_information_generator}"
        "${LADYBIRD_SOURCE_DIR}" "${CMAKE_BINARY_DIR}" "${build_information_header}"
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        --depfile "${build_information_depfile}"
        --stamp "${build_information_stamp}"
    COMMAND
        ${CMAKE_COMMAND} -E touch "${build_information_stamp}"
    DEPENDS
        "${build_information_generator}"
        "${LADYBIRD_BUILD_HEAD}"
        "${LADYBIRD_BUILD_HEAD_LOG}"
        "${CMAKE_BINARY_DIR}/CMakeCache.txt"
        "${CMAKE_BINARY_DIR}/compile_commands.json"
    DEPFILE "${build_information_depfile}"
    OUTPUT "${build_information_stamp}"
    BYPRODUCTS "${build_information_header}"
    VERBATIM
)
add_custom_target(generate_build_information DEPENDS ${build_information_stamp})

ladybird_lib(LibWebView webview EXPLICIT_SYMBOL_EXPORT)
add_dependencies(LibWebView generate_build_information)
target_link_libraries(LibWebView PUBLIC LibCompositing LibSandbox LibWebCommon)
target_link_libraries(LibWebView PRIVATE LibCore LibCrypto LibDatabase LibDevTools LibFileSystem LibGfx LibHTTP LibImageDecoderClient LibIPC LibRequests LibMedia LibMediaClient LibUnicode LibURL LibTextCodec LibThreading SDL3::SDL3)

if (ENABLE_CRANELIFT_JIT)
    target_link_libraries(LibWebView PRIVATE LibWasmCompilerClient)
    target_compile_definitions(LibWebView PUBLIC HAVE_WASM_COMPILER_SERVICE=1)
endif()

if (APPLE)
    target_link_libraries(LibWebView PRIVATE "-framework Carbon")
endif()

# Third-party
if (HAS_FONTCONFIG)
    target_link_libraries(LibWebView PRIVATE Fontconfig::Fontconfig)
endif()

if (ENABLE_INSTALL_HEADERS)
    foreach(header ${GENERATED_SOURCES})
        get_filename_component(extension ${header} EXT)
        if (NOT "${extension}" STREQUAL ".h")
            continue()
        endif()

        get_filename_component(subdirectory ${header} DIRECTORY)
        string(REGEX REPLACE "^\\.\\./\\.\\./" "" subdirectory "${subdirectory}")

        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${subdirectory}")
    endforeach()
endif()
