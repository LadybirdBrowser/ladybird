set(SOURCES
    Color/ColorConverter.cpp
    Color/ColorPrimaries.cpp
    Color/TransferCharacteristics.cpp
    Containers/Matroska/MatroskaDemuxer.cpp
    Containers/Matroska/Reader.cpp
    FFmpeg/FFmpegVideoDecoder.cpp
    PlaybackManager.cpp
    VideoFrame.cpp
)

serenity_lib(LibMedia media)
target_link_libraries(LibMedia PRIVATE LibCore LibIPC LibGfx LibThreading)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
target_include_directories(LibMedia PRIVATE ${AVCODEC_INCLUDE_DIRS})
target_link_directories(LibMedia PRIVATE ${AVCODEC_LIBRARY_DIRS})
target_link_libraries(LibMedia PRIVATE ${AVCODEC_LIBRARIES})
