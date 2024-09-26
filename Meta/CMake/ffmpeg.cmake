include_guard()

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat)

if (AVCODEC_FOUND AND AVFORMAT_FOUND)
    set(HAS_FFMPEG ON CACHE BOOL "" FORCE)
    add_compile_definitions(USE_FFMPEG=1)
endif()
