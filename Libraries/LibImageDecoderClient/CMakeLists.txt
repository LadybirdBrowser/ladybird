set(SOURCES
    Client.cpp
)

if (NOT SERENITYOS)
    compile_ipc(../../Services/ImageDecoder/ImageDecoderClient.ipc ../../Services/ImageDecoder/ImageDecoderClientEndpoint.h)
    compile_ipc(../../Services/ImageDecoder/ImageDecoderServer.ipc ../../Services/ImageDecoder/ImageDecoderServerEndpoint.h)
endif()

set(GENERATED_SOURCES
    ../../Services/ImageDecoder/ImageDecoderClientEndpoint.h
    ../../Services/ImageDecoder/ImageDecoderServerEndpoint.h
)

serenity_lib(LibImageDecoderClient imagedecoderclient)
target_link_libraries(LibImageDecoderClient PRIVATE LibCore LibIPC LibGfx)
