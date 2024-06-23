set(SOURCES
    ConnectionFromClient.cpp
    DedicatedWorkerHost.cpp
    PageHost.cpp
    main.cpp
)

set(GENERATED_SOURCES
    ../Libraries/LibWeb/Worker/WebWorkerClientEndpoint.h
    ../Libraries/LibWeb/Worker/WebWorkerServerEndpoint.h
)

serenity_bin(WebWorker)
target_link_libraries(WebWorker PRIVATE LibCore LibFileSystem LibGfx LibIPC LibJS LibWeb LibWebView LibUnicode LibMain LibURL)
