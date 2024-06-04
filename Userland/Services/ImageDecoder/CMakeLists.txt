compile_ipc(ImageDecoderServer.ipc ImageDecoderServerEndpoint.h)
compile_ipc(ImageDecoderClient.ipc ImageDecoderClientEndpoint.h)

set(SOURCES
    ConnectionFromClient.cpp
    main.cpp
)

set(GENERATED_SOURCES
    ImageDecoderServerEndpoint.h
    ImageDecoderClientEndpoint.h
)

serenity_bin(ImageDecoder)
target_link_libraries(ImageDecoder PRIVATE LibCore LibGfx LibIPC LibMain LibThreading)
