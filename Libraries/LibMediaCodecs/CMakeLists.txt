set(SOURCES
    AudioDecoderRegistry.cpp
    CodecParameters.cpp
    Codecs/AAC.cpp
    Codecs/AV1.cpp
    Codecs/H264.cpp
    Codecs/H265.cpp
    Codecs/MPEG4Audio.cpp
    Codecs/VP9.cpp
    Color/CodingIndependentCodePoints.cpp
    DecoderCapabilities.cpp
    DecoderError.cpp
    FFmpeg/FFmpegFunctions.cpp
    FFmpeg/FFmpegHelpers.cpp
    VideoDecoderRegistry.cpp
    VideoFrame.cpp
    VideoFrameHandle.cpp
    VideoFramePool.cpp
    VideoSurface.cpp
)

if (LINUX)
    list(APPEND SOURCES FFmpeg/SystemFFmpeg.cpp)
endif()

ladybird_lib(LibMediaCodecs mediacodecs EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibMediaCodecs PRIVATE LibCore LibGfx LibIPC)
target_link_libraries(LibMediaCodecs PUBLIC LibAudio)

if (NOT ANDROID)
    target_link_libraries(LibMediaCodecs PRIVATE PkgConfig::AVCODEC PkgConfig::AVUTIL)
else()
    target_include_directories(LibMediaCodecs PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(LibMediaCodecs PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(LibMediaCodecs PRIVATE ${FFMPEG_LIBRARIES})
endif()

if (LINUX)
    target_link_libraries(LibMediaCodecs PRIVATE ${CMAKE_DL_LIBS})
endif()
