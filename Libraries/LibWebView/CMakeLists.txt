include(fontconfig)

set(SOURCES
    Application.cpp
    Attribute.cpp
    BrowserProcess.cpp
    ConsoleOutput.cpp
    CookieJar.cpp
    Database.cpp
    DOMNodeProperties.cpp
    HelperProcess.cpp
    Mutation.cpp
    Plugins/FontPlugin.cpp
    Plugins/ImageCodecPlugin.cpp
    Process.cpp
    ProcessHandle.cpp
    ProcessManager.cpp
    SearchEngine.cpp
    Settings.cpp
    SiteIsolation.cpp
    SourceHighlighter.cpp
    URL.cpp
    UserAgent.cpp
    Utilities.cpp
    ViewImplementation.cpp
    WebContentClient.cpp
    WebUI.cpp
    WebUI/ProcessesUI.cpp
    WebUI/SettingsUI.cpp
)

if (APPLE)
    list(APPEND SOURCES MachPortServer.cpp)
endif()

if (ENABLE_QT)
    list(APPEND SOURCES
        EventLoop/EventLoopImplementationQt.cpp
        EventLoop/EventLoopImplementationQtEventTarget.cpp
    )

    set(CMAKE_AUTOMOC ON)
    find_package(Qt6 REQUIRED COMPONENTS Core)
elseif (APPLE)
    list(APPEND SOURCES
        EventLoop/EventLoopImplementationMacOS.mm
    )
endif()

set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

embed_as_string(
    "NativeStyleSheetSource.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Native.css"
    "NativeStyleSheetSource.cpp"
    "native_stylesheet_source"
    NAMESPACE "WebView"
)

compile_ipc(UIProcessServer.ipc UIProcessServerEndpoint.h)
compile_ipc(UIProcessClient.ipc UIProcessClientEndpoint.h)

if (NOT APPLE AND NOT CMAKE_INSTALL_LIBEXECDIR STREQUAL "libexec")
    set_source_files_properties(Utilities.cpp PROPERTIES COMPILE_DEFINITIONS LADYBIRD_LIBEXECDIR="${CMAKE_INSTALL_LIBEXECDIR}")
endif()

set(GENERATED_SOURCES
    ${GENERATED_SOURCES}
    ../../Services/RequestServer/RequestClientEndpoint.h
    ../../Services/RequestServer/RequestServerEndpoint.h
    ../../Services/WebContent/WebContentClientEndpoint.h
    ../../Services/WebContent/WebContentServerEndpoint.h
    ../../Services/WebContent/WebDriverClientEndpoint.h
    ../../Services/WebContent/WebDriverServerEndpoint.h
    ../../Services/WebContent/WebUIClientEndpoint.h
    ../../Services/WebContent/WebUIServerEndpoint.h
    NativeStyleSheetSource.cpp
    UIProcessClientEndpoint.h
    UIProcessServerEndpoint.h
)

serenity_lib(LibWebView webview)
target_link_libraries(LibWebView PRIVATE LibCore LibDevTools LibFileSystem LibGfx LibImageDecoderClient LibIPC LibRequests LibJS LibWeb LibUnicode LibURL LibSyntax)

if (APPLE)
    target_link_libraries(LibWebView PRIVATE LibThreading)
endif()

# Third-party
find_package(SQLite3 REQUIRED)
target_link_libraries(LibWebView PRIVATE SQLite::SQLite3)

if (ENABLE_QT)
    target_link_libraries(LibWebView PRIVATE Qt::Core)
elseif (APPLE)
    target_link_libraries(LibWebView PRIVATE "-framework Cocoa")
endif()

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
