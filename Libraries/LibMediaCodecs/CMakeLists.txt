set(SOURCES
    AudioDecoderRegistry.cpp
    CodecParameters.cpp
    Codecs/AAC.cpp
    Codecs/AV1.cpp
    Codecs/DolbyVision.cpp
    Codecs/H264.cpp
    Codecs/H265.cpp
    Codecs/HDR10Plus.cpp
    Codecs/HighDynamicRangeMetadataTracker.cpp
    Codecs/MPEG4Audio.cpp
    Codecs/SupplementalEnhancementInformation.cpp
    Codecs/VP9.cpp
    Color/CodingIndependentCodePoints.cpp
    Color/ColorMetadata.cpp
    DecoderCapabilities.cpp
    DecoderError.cpp
    FFmpeg/FFmpegFunctions.cpp
    FFmpeg/FFmpegHelpers.cpp
    FFmpeg/FFmpegVideoDecoder.cpp
    FFmpeg/MallocTuning.cpp
    FFmpeg/VideoFramesAwaitingOutput.cpp
    HardwareVideoDecoders.cpp
    NonReferenceFrameDetector.cpp
    VideoDecoderRegistry.cpp
    VideoDecoderServerConnection.cpp
    VideoFrame.cpp
    VideoFrameMetadata.cpp
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
    list(APPEND SOURCES
        FFmpeg/FFmpegVulkanVideoDecoder.cpp
        FFmpeg/FFmpegVulkanVideoDecoderImplementation.cpp
        Vulkan/VulkanExportableImagePool.cpp
        Vulkan/VulkanVideoDevice.cpp
    )
endif()

# The Vulkan decoder reaches into FFmpeg's structs, so the code that does is also compiled against the headers of each
# other FFmpeg release, to decode with a system FFmpeg of that version.
set(SYSTEM_FFMPEG_HEADER_MAJORS "")
if (LINUX AND NOT ANDROID AND USE_VULKAN_DMABUF_IMAGES)
    include(system_ffmpeg_headers)
endif()
foreach(major IN LISTS SYSTEM_FFMPEG_HEADER_MAJORS)
    set(objects LibMediaCodecsForLibAVCodec${major})
    add_library(${objects} OBJECT
        FFmpeg/FFmpegFunctions.cpp
        FFmpeg/FFmpegHelpers.cpp
        FFmpeg/FFmpegVulkanVideoDecoderImplementation.cpp
    )
    ladybird_library_setup(${objects})
    target_compile_definitions(${objects} PRIVATE LADYBIRD_SYSTEM_FFMPEG_NAMESPACE=AVCodec${major} LibMediaCodecs_EXPORTS)
    set_target_properties(${objects} PROPERTIES DEFINE_SYMBOL LibMediaCodecs_EXPORTS)
    target_include_directories(${objects} SYSTEM BEFORE PRIVATE "${SYSTEM_FFMPEG_${major}_INCLUDE_DIRECTORY}")
    target_compile_options(${objects} PRIVATE -fvisibility=hidden)
    target_link_libraries(${objects} PRIVATE LibCore LibGfx LibIPC LibPlatformFeatures LibThreading LibAudio Vulkan::Vulkan)
    list(APPEND SOURCES $<TARGET_OBJECTS:${objects}>)
endforeach()

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
target_link_libraries(LibMediaCodecs PRIVATE LibCore LibGfx LibIPC LibPlatformFeatures LibThreading)
target_link_libraries(LibMediaCodecs PUBLIC LibAudio)

if (ANDROID)
    target_include_directories(LibMediaCodecs PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(LibMediaCodecs PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(LibMediaCodecs PRIVATE ${FFMPEG_LIBRARIES})
elseif (LINUX)
    # A system FFmpeg may be loaded beside the bundled one, so the bundled one is loaded with RTLD_LOCAL rather than
    # linked, which keeps its symbols from taking the place of the system one's.
    ladybird_use_bundled_ffmpeg_headers(LibMediaCodecs)
    target_compile_definitions(LibMediaCodecs PRIVATE
        LADYBIRD_BUNDLED_LIBAVCODEC="${LADYBIRD_BUNDLED_AVCODEC_SONAME}"
        LADYBIRD_BUNDLED_LIBSWRESAMPLE="${LADYBIRD_BUNDLED_LIBSWRESAMPLE_SONAME}"
    )
else()
    target_link_libraries(LibMediaCodecs PRIVATE PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::LIBSWRESAMPLE)
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

    set(system_ffmpeg_header_majors "")
    foreach(major IN LISTS SYSTEM_FFMPEG_HEADER_MAJORS)
        string(APPEND system_ffmpeg_header_majors " F(${major})")
    endforeach()
    file(CONFIGURE OUTPUT SystemFFmpegHeaderMajors.h CONTENT "#pragma once\n\n#define ENUMERATE_SYSTEM_FFMPEG_HEADER_MAJORS(F)${system_ffmpeg_header_majors}\n" @ONLY)
endif()
