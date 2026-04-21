include_guard()

find_package(PkgConfig REQUIRED)

find_package(mimalloc CONFIG REQUIRED)

# AK dependencies
find_package(Backtrace)
find_package(cpptrace CONFIG)
find_package(FastFloat CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(simdutf REQUIRED)

# LibGfx dependencies
find_package(harfbuzz REQUIRED)
find_package(JPEG REQUIRED)
find_package(LIBAVIF REQUIRED)
find_package(PNG REQUIRED)
find_package(WebP REQUIRED)

pkg_check_modules(WOFF2 REQUIRED IMPORTED_TARGET libwoff2dec)

# TODO: Figure out if we can do this the same way on all platforms
if (NOT ANDROID)
    pkg_check_modules(Jxl REQUIRED IMPORTED_TARGET libjxl)

    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(LIBSWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
else()
    find_package(libjxl REQUIRED)
    find_package(hwy REQUIRED)

    find_package(FFMPEG REQUIRED)
endif()

if (NOT APPLE AND NOT ANDROID AND NOT WIN32)
    find_package(Fontconfig REQUIRED)
    set(HAS_FONTCONFIG ON CACHE BOOL "" FORCE)
    add_cxx_compile_definitions(USE_FONTCONFIG=1)
endif()

if (NOT APPLE)
    find_package(VulkanHeaders CONFIG QUIET)
    find_package(Vulkan QUIET)
    if (VulkanHeaders_FOUND AND Vulkan_FOUND)
        set(HAS_VULKAN ON CACHE BOOL "" FORCE)
        add_cxx_compile_definitions(USE_VULKAN=1)

        # Sharable Vulkan images are currently only implemented on Linux and BSDs
        if ((LINUX AND NOT ANDROID) OR BSD)
            set(USE_VULKAN_DMABUF_IMAGES ON CACHE BOOL "" FORCE)
            add_cxx_compile_definitions(USE_VULKAN_DMABUF_IMAGES=1)
        endif()
    endif()
endif()

find_package(CURL REQUIRED)
find_package(ICU 78.2 EXACT REQUIRED COMPONENTS data i18n uc)
find_package(LibXml2 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(libtommath REQUIRED IMPORTED_TARGET libtommath)

find_package(unofficial-angle CONFIG)
if(unofficial-angle_FOUND)
    set(ANGLE_TARGETS unofficial::angle::libEGL unofficial::angle::libGLESv2)
else()
    pkg_check_modules(angle REQUIRED IMPORTED_TARGET angle)
    set(ANGLE_TARGETS PkgConfig::angle)
endif()

if (WIN32)
    find_package(pthread REQUIRED)
    find_package(mman REQUIRED)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

find_package(unofficial-skia CONFIG)
if(unofficial-skia_FOUND)
    set(SKIA_TARGET unofficial::skia::skia)
    if (HAS_FONTCONFIG AND NOT WIN32)
        set(CMAKE_LINK_GROUP_USING_no_as_needed_SUPPORTED TRUE CACHE BOOL "Link group using no-as-needed supported")
        set(CMAKE_LINK_GROUP_USING_no_as_needed "LINKER:--push-state,--no-as-needed" "LINKER:--pop-state" CACHE STRING "Link group using no-as-needed")
        set_property(TARGET unofficial::skia::skia APPEND PROPERTY INTERFACE_LINK_LIBRARIES "$<LINK_GROUP:no_as_needed,Fontconfig::Fontconfig>")
    endif()
    if (ANDROID)
        # FIXME: Submit a proper patch to vcpkg in order not to bring host's libc++ when compiling for Android
        get_target_property(link_libs unofficial::skia::skia INTERFACE_LINK_LIBRARIES)
        set(filtered_libs)
        foreach(lib ${link_libs})
            if (NOT lib MATCHES "lib/libc\\+\\+.so$")
                list(APPEND filtered_libs ${lib})
            endif()
        endforeach()
        set_property(TARGET unofficial::skia::skia PROPERTY INTERFACE_LINK_LIBRARIES ${filtered_libs})
    endif()
else()
    # Get skia version from vcpkg.json
    file(READ ${LADYBIRD_SOURCE_DIR}/vcpkg.json VCPKG_DOT_JSON)
    string(JSON VCPKG_OVERRIDES_LENGTH LENGTH ${VCPKG_DOT_JSON} overrides)
    MATH(EXPR VCPKG_OVERRIDES_END_RANGE "${VCPKG_OVERRIDES_LENGTH}-1")
    foreach(IDX RANGE ${VCPKG_OVERRIDES_END_RANGE})
      string(JSON VCPKG_OVERRIDE_NAME GET ${VCPKG_DOT_JSON} overrides ${IDX} name)
      if(VCPKG_OVERRIDE_NAME STREQUAL "skia")
        string(JSON SKIA_REQUIRED_VERSION GET ${VCPKG_DOT_JSON} overrides ${IDX} version)
        string(REGEX MATCH "[0-9]+" SKIA_REQUIRED_VERSION ${SKIA_REQUIRED_VERSION})
      endif()
    endforeach()

    pkg_check_modules(skia skia=${SKIA_REQUIRED_VERSION} REQUIRED IMPORTED_TARGET skia)
    set(SKIA_TARGET PkgConfig::skia)
    set_property(TARGET PkgConfig::skia APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS "SKCMS_DLL")
endif()
add_library(skia ALIAS ${SKIA_TARGET})
