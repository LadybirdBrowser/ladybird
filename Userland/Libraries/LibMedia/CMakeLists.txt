set(SOURCES
    Color/ColorConverter.cpp
    Color/ColorPrimaries.cpp
    Color/TransferCharacteristics.cpp
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    PlaybackManager.cpp
    VideoFrame.cpp
    Video/VP9/Decoder.cpp
    Video/VP9/Parser.cpp
    Video/VP9/ProbabilityTables.cpp
    Video/VP9/SyntaxElementCounter.cpp
    Video/VP9/TreeParser.cpp
)

serenity_lib(LibMedia media)
target_link_libraries(LibMedia PRIVATE LibCore LibIPC LibGfx LibThreading)
