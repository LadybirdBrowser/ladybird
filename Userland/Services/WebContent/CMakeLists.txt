compile_ipc(WebContentServer.ipc WebContentServerEndpoint.h)
compile_ipc(WebContentClient.ipc WebContentClientEndpoint.h)

compile_ipc(WebDriverClient.ipc WebDriverClientEndpoint.h)
compile_ipc(WebDriverServer.ipc WebDriverServerEndpoint.h)

set(SOURCES
    ConnectionFromClient.cpp
    ConsoleGlobalEnvironmentExtensions.cpp
    ImageCodecPluginSerenity.cpp
    PageClient.cpp
    PageHost.cpp
    WebContentConsoleClient.cpp
    WebDriverConnection.cpp
    main.cpp
)

set(GENERATED_SOURCES
    WebContentClientEndpoint.h
    WebContentServerEndpoint.h
    WebDriverClientEndpoint.h
    WebDriverServerEndpoint.h
)

serenity_bin(WebContent)
target_link_libraries(WebContent PRIVATE LibCore LibFileSystem LibIPC LibGfx LibAudio LibImageDecoderClient LibJS LibWebView LibWeb LibUnicode LibMain LibURL)
