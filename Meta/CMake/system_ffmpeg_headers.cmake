# The Vulkan video decoder reaches into FFmpeg's structs, which are laid out differently in each major version. These
# are the headers of the releases other than the one Ladybird is built against, so that it can do so in a system
# FFmpeg of any of those versions.

include(${CMAKE_CURRENT_LIST_DIR}/utils.cmake)

set(SYSTEM_FFMPEG_HEADERS_CACHE_PATH "${LADYBIRD_CACHE_DIR}/FFmpegHeaders" CACHE PATH "Download location for FFmpeg release archives")

# The libavcodec major version, the release whose headers stand for it, and the SHA-256 of that release's archive.
set(system_ffmpeg_releases
    "60|6.1.6|d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a"
    "61|7.1.5|de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"
    "62|8.1.3|7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3"
    "63|9.0.2|8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
)

# pkg-config's answer for the version is cached from the first configuration, so the headers themselves say which
# version Ladybird is built against.
find_file(system_ffmpeg_built_against_version_header libavcodec/version_major.h HINTS ${AVCODEC_INCLUDE_DIRS} NO_CACHE REQUIRED)
file(STRINGS "${system_ffmpeg_built_against_version_header}" system_ffmpeg_built_against_major REGEX "#define LIBAVCODEC_VERSION_MAJOR")
string(REGEX MATCH "[0-9]+" system_ffmpeg_built_against_major "${system_ffmpeg_built_against_major}")

set(SYSTEM_FFMPEG_HEADER_MAJORS "")
set(system_ffmpeg_missing_releases "")
foreach(release IN LISTS system_ffmpeg_releases)
    string(REPLACE "|" ";" release "${release}")
    list(GET release 0 major)
    list(GET release 1 version)
    list(GET release 2 sha256)
    if (major STREQUAL system_ffmpeg_built_against_major)
        continue()
    endif()

    set(archive "${SYSTEM_FFMPEG_HEADERS_CACHE_PATH}/ffmpeg-${version}.tar.xz")
    if (NOT EXISTS "${archive}" AND NOT ENABLE_NETWORK_DOWNLOADS)
        list(APPEND system_ffmpeg_missing_releases "${version}")
        continue()
    endif()
    download_file("https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz" "${archive}" SHA256 "${sha256}")

    set(include_directory "${CMAKE_BINARY_DIR}/SystemFFmpegHeaders/${version}")
    if (NOT EXISTS "${include_directory}/libavutil/avconfig.h")
        file(REMOVE_RECURSE "${include_directory}" "${include_directory}.tmp")
        file(ARCHIVE_EXTRACT
            INPUT "${archive}"
            DESTINATION "${include_directory}.tmp"
            PATTERNS "ffmpeg-${version}/libavcodec/*.h" "ffmpeg-${version}/libavutil/*.h"
        )
        file(RENAME "${include_directory}.tmp/ffmpeg-${version}" "${include_directory}")
        file(REMOVE_RECURSE "${include_directory}.tmp")
        # FFmpeg's build generates this header, which only describes the machine.
        if (CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
            set(system_ffmpeg_big_endian 1)
        else()
            set(system_ffmpeg_big_endian 0)
        endif()
        file(WRITE "${include_directory}/libavutil/avconfig.h" "#define AV_HAVE_BIGENDIAN ${system_ffmpeg_big_endian}\n#define AV_HAVE_FAST_UNALIGNED 0\n")
    endif()

    list(APPEND SYSTEM_FFMPEG_HEADER_MAJORS "${major}")
    set(SYSTEM_FFMPEG_${major}_INCLUDE_DIRECTORY "${include_directory}")
endforeach()

if (system_ffmpeg_missing_releases)
    list(JOIN system_ffmpeg_missing_releases ", " system_ffmpeg_missing_releases)
    message(WARNING "The archives of FFmpeg ${system_ffmpeg_missing_releases} are not in ${SYSTEM_FFMPEG_HEADERS_CACHE_PATH}, and downloads are disabled. Hardware video decoding through a system FFmpeg of those versions will be unavailable.")
endif()
