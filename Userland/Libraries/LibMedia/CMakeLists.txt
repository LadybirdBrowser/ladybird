set(SOURCES
    Color/ColorConverter.cpp
    Color/ColorPrimaries.cpp
    Color/TransferCharacteristics.cpp
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    PlaybackManager.cpp
    VideoFrame.cpp
)

if (NOT ANDROID)
    list(APPEND SOURCES FFmpeg/FFmpegVideoDecoder.cpp)
else()
    list(APPEND SOURCES FFmpeg/FFmpegVideoDecoderStub.cpp)
endif()

serenity_lib(LibMedia media)
target_link_libraries(LibMedia PRIVATE LibCore LibIPC LibGfx LibThreading)

if (NOT ANDROID)
    # Third-party
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    target_link_libraries(LibMedia PRIVATE PkgConfig::AVCODEC)
endif()
