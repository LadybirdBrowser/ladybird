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
    FFmpeg/FFmpegVideoDecoder.cpp
    HardwareVideoDecoders.cpp
    NonReferenceFrameDetector.cpp
    VideoDecoderRegistry.cpp
    VideoDecoderServerConnection.cpp
    VideoFrame.cpp
    VideoFrameHandle.cpp
    VideoFramePool.cpp
    VideoSurface.cpp
)

if (LINUX)
    list(APPEND SOURCES FFmpeg/SystemFFmpeg.cpp)
endif()

if (APPLE)
    list(APPEND SOURCES VideoToolbox/VideoToolboxVideoDecoder.mm)
endif()

if (WIN32)
    list(APPEND SOURCES
        WindowsMediaFoundation/WindowsMediaFoundation.cpp
        WindowsMediaFoundation/WindowsMediaFoundationVideoDecoder.cpp
    )
endif()

if (USE_VULKAN_DMABUF_IMAGES)
    list(APPEND SOURCES FFmpeg/FFmpegVulkanVideoDecoder.cpp Vulkan/VulkanVideoDevice.cpp)
endif()

set(GENERATED_SOURCES
    VideoDecoderClientEndpoint.h
    VideoDecoderServerEndpoint.h
    ../../Services/VideoDecoderServer/VideoDecoderServerControlClientEndpoint.h
    ../../Services/VideoDecoderServer/VideoDecoderServerControlEndpoint.h
)

compile_ipc(VideoDecoderClient.ipc VideoDecoderClientEndpoint.h)
compile_ipc(VideoDecoderServer.ipc VideoDecoderServerEndpoint.h)
compile_ipc(../../Services/VideoDecoderServer/VideoDecoderServerControl.ipc ../../Services/VideoDecoderServer/VideoDecoderServerControlEndpoint.h)
compile_ipc(../../Services/VideoDecoderServer/VideoDecoderServerControlClient.ipc ../../Services/VideoDecoderServer/VideoDecoderServerControlClientEndpoint.h)

ladybird_lib(LibMediaCodecs mediacodecs EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibMediaCodecs PRIVATE LibCore LibGfx LibIPC LibThreading)
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

if (APPLE)
    target_link_libraries(LibMediaCodecs PRIVATE "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
endif()

if (WIN32)
    target_link_libraries(LibMediaCodecs PRIVATE mfplat mfuuid)
endif()

if (USE_VULKAN_DMABUF_IMAGES)
    target_link_libraries(LibMediaCodecs PRIVATE Vulkan::Vulkan)
endif()
