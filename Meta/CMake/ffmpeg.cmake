include_guard()

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat)

if (AVCODEC_FOUND AND AVFORMAT_FOUND)
    set(HAS_FFMPEG ON CACHE BOOL "" FORCE)
    add_compile_definitions(USE_FFMPEG=1)
    if (AVCODEC_VERSION VERSION_GREATER_EQUAL "59.24.100")
        add_compile_definitions(USE_FFMPEG_CH_LAYOUT=1)
    endif()
endif()
