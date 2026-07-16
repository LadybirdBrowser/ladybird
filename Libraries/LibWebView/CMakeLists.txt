set(SOURCES
    Application.cpp
    Attribute.cpp
    Autocomplete.cpp
    AutocompleteMuxer.cpp
    AutocompleteRanker.cpp
    AutocompleteService.cpp
    BookmarkStore.cpp
    CanonicalNavigable.cpp
    CanonicalTraversable.cpp
    BrowserProcess.cpp
    CompositorClient.cpp
    CompositorConnection.cpp
    CompositorHostBase.cpp
    ConsoleOutput.cpp
    CookieJar.cpp
    DictionaryLookup.cpp
    DOMNodeProperties.cpp
    DownloadPresentation.cpp
    FileDownloader.cpp
    HeadlessWebView.cpp
    HelperProcess.cpp
    HistoryDebug.cpp
    HistoryStore.cpp
    HSTSStore.cpp
    Menu.cpp
    Mutation.cpp
    Omnibox.cpp
    OmniboxEngagement.cpp
    Plugins/ImageCodecPlugin.cpp
    PrivateBrowsing.cpp
    Process.cpp
    ProcessHandle.cpp
    ProcessManager.cpp
    ProcessMonitor.cpp
    Profile.cpp
    SearchEngine.cpp
    SessionHistory.cpp
    Settings.cpp
    SiteIsolation.cpp
    SiteIsolationManager.cpp
    SourceHighlighter.cpp
    StorageJar.cpp
    URL.cpp
    UserAgent.cpp
    Utilities.cpp
    ViewImplementation.cpp
    WebContentClient.cpp
    WebUI.cpp
    WebUI/BookmarksUI.cpp
    WebUI/DownloadsUI.cpp
    WebUI/HistoryUI.cpp
    WebUI/ProcessesUI.cpp
    WebUI/SettingsUI.cpp
    WebUI/VersionUI.cpp
    WebWorkerClient.cpp
    WorkerProcessManager.cpp
)

if (APPLE)
    list(APPEND SOURCES
        PlatformColorsMacOS.cpp
    )
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
    ../../Services/Compositor/CompositorWebContentClientEndpoint.h
    ../../Services/Compositor/CompositorWebContentServerEndpoint.h
    ../../Services/RequestServer/RequestClientEndpoint.h
    ../../Services/RequestServer/RequestServerEndpoint.h
    ../../Services/WebContent/WebContentClientEndpoint.h
    ../../Services/WebContent/WebContentServerEndpoint.h
    ../../Services/WebContent/WebDriverClientEndpoint.h
    ../../Services/WebContent/WebDriverServerEndpoint.h
    ../../Services/WebContent/WebUIClientEndpoint.h
    ../../Services/WebContent/WebUIServerEndpoint.h
    UIProcessClientEndpoint.h
    UIProcessServerEndpoint.h
)

compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorControlServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorControlServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentClient.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/Compositor/CompositorWebContentServer.ipc ${CMAKE_BINARY_DIR}/Services/Compositor/CompositorWebContentServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebDriverClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebDriverClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebDriverServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebDriverServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebUIClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebUIClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebUIServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebUIServerEndpoint.h)

ladybird_lib(LibWebView webview EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWebView PRIVATE LibCore LibCrypto LibDatabase LibDevTools LibFileSystem LibGfx LibHTTP LibImageDecoderClient LibIPC LibRequests LibJS LibMedia LibWeb LibUnicode LibURL LibSync LibSyntax LibTextCodec LibThreading)

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
