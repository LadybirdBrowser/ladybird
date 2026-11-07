set(SOURCES
    Client.cpp
)

if (NOT SERENITYOS)
    compile_ipc(../../Services/ImageDecoder/ImageDecoderClient.ipc ../../Services/ImageDecoder/ImageDecoderClientEndpoint.h)
    compile_ipc(../../Services/ImageDecoder/ImageDecoderControl.ipc ../../Services/ImageDecoder/ImageDecoderControlEndpoint.h)
    compile_ipc(../../Services/ImageDecoder/ImageDecoderControlClient.ipc ../../Services/ImageDecoder/ImageDecoderControlClientEndpoint.h)
    compile_ipc(../../Services/ImageDecoder/ImageDecoderServer.ipc ../../Services/ImageDecoder/ImageDecoderServerEndpoint.h)
endif()

set(GENERATED_SOURCES
    ../../Services/ImageDecoder/ImageDecoderClientEndpoint.h
    ../../Services/ImageDecoder/ImageDecoderControlClientEndpoint.h
    ../../Services/ImageDecoder/ImageDecoderControlEndpoint.h
    ../../Services/ImageDecoder/ImageDecoderServerEndpoint.h
)

ladybird_lib(LibImageDecoderClient imagedecoderclient)
target_link_libraries(LibImageDecoderClient PRIVATE LibCore LibIPC LibGfx)
