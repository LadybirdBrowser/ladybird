set(SOURCES
    Application.cpp
    ApplyHistoryStep.cpp
    Attribute.cpp
    Autocomplete.cpp
    AutocompleteMuxer.cpp
    AutocompleteRanker.cpp
    AutocompleteService.cpp
    BlobURLStore.cpp
    BrowsingSession.cpp
    BookmarkStore.cpp
    CanonicalBrowsingContext.cpp
    CanonicalBrowsingContextGroup.cpp
    CanonicalNavigable.cpp
    CanonicalTraversable.cpp
    BrowserProcess.cpp
    CompositorClient.cpp
    CompositorFontServiceConnection.cpp
    CompositorConnection.cpp
    CompositorHostBase.cpp
    ConsoleOutput.cpp
    CookieJar.cpp
    CrashReport.cpp
    Debugger.cpp
    DictionaryLookup.cpp
    DOMNodeProperties.cpp
    DownloadPresentation.cpp
    DownloadSegmentation.cpp
    DownloadStore.cpp
    ExternalURLHandler.cpp
    FaviconStore.cpp
    FileDownloader.cpp
    FontService.cpp
    Geolocation.cpp
    HeadlessWebView.cpp
    HelperProcess.cpp
    HistoryDebug.cpp
    HistoryStore.cpp
    HSTSStore.cpp
    Menu.cpp
    Mutation.cpp
    NavigationLoader.cpp
    Omnibox.cpp
    OmniboxEngagement.cpp
    Plugins/ImageCodecPlugin.cpp
    Process.cpp
    ProcessHandle.cpp
    ProcessManager.cpp
    TabPerformanceStats.cpp
    TabPerformanceMonitor.cpp
    ProcessMonitor.cpp
    Profile.cpp
    SearchEngine.cpp
    SessionHistory.cpp
    SessionHistorySnapshotStorage.cpp
    SessionHistoryTraversalQueue.cpp
    SessionStore.cpp
    Settings.cpp
    SiteCompatibility.cpp
    SiteIsolation.cpp
    SiteIsolationManager.cpp
    SourceHighlighter.cpp
    StorageJar.cpp
    URL.cpp
    UserAgent.cpp
    Utilities.cpp
    ViewImplementation.cpp
    WebContentClient.cpp
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
        PlatformColorsMacOS.cpp
    )
elseif (LINUX)
    list(APPEND SOURCES CrashReportPOSIX.cpp)
endif()

set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

compile_ipc(UIProcessServer.ipc UIProcessServerEndpoint.h)
compile_ipc(UIProcessClient.ipc UIProcessClientEndpoint.h)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Services/Compositor)

if (NOT APPLE AND NOT CMAKE_INSTALL_LIBEXECDIR STREQUAL "libexec")
    set_source_files_properties(Utilities.cpp PROPERTIES COMPILE_DEFINITIONS LADYBIRD_LIBEXECDIR="${CMAKE_INSTALL_LIBEXECDIR}")
endif()

set(GENERATED_SOURCES
    ${GENERATED_SOURCES}
    ../../Services/Compositor/CompositorControlClientEndpoint.h
    ../../Services/Compositor/CompositorControlServerEndpoint.h
    ../../Services/Compositor/CompositorFontClientEndpoint.h
    ../../Services/Compositor/CompositorFontServerEndpoint.h
    ../../Services/Compositor/CompositorWebContentClientEndpoint.h
    ../../Services/Compositor/CompositorWebContentServerEndpoint.h
    ../../Services/RequestServer/RequestClientEndpoint.h
    ../../Services/RequestServer/RequestServerEndpoint.h
    ../../Services/WebContent/WebContentClientEndpoint.h
    ../../Services/WebContent/WebContentServerEndpoint.h
    ../../Services/WebDriver/WebDriverBrowserClientEndpoint.h
    ../../Services/WebDriver/WebDriverBrowserServerEndpoint.h
    ../../Services/WebContent/WebUIClientEndpoint.h
    ../../Services/WebContent/WebUIServerEndpoint.h
    UIProcessClientEndpoint.h
    UIProcessServerEndpoint.h
)

compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorFontClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorFontClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorFontServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorFontServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebDriver/WebDriverBrowserClient.ipc ${CMAKE_BINARY_DIR}/Services/WebDriver/WebDriverBrowserClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebDriver/WebDriverBrowserServer.ipc ${CMAKE_BINARY_DIR}/Services/WebDriver/WebDriverBrowserServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebUIClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebUIClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebUIServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebUIServerEndpoint.h)

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
target_link_libraries(LibWebView PRIVATE LibCore LibCrypto LibDatabase LibDevTools LibFileSystem LibGC LibGfx LibHTTP LibImageDecoderClient LibIPC LibRequests LibJS LibMedia LibWeb LibUnicode LibURL LibSync LibSyntax LibTextCodec LibThreading)

if (ENABLE_CRANELIFT_JIT)
    target_link_libraries(LibWebView PRIVATE LibWasmCompilerClient)
    target_compile_definitions(LibWebView PUBLIC HAVE_WASM_COMPILER_SERVICE=1)
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
