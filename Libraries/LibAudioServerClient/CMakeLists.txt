set(SOURCES
    Client.cpp
)

set(GENERATED_SOURCES
    ../../Services/AudioServer/AudioServerClientEndpoint.h
    ../../Services/AudioServer/AudioServerServerEndpoint.h
)

ladybird_lib(LibAudioServerClient audioserverclient)
target_link_libraries(LibAudioServerClient PRIVATE LibCore LibIPC)
