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
else()
    find_package(libjxl REQUIRED)
    find_package(hwy REQUIRED)
endif()

find_package(CURL REQUIRED)
find_package(ICU 78.2 EXACT REQUIRED COMPONENTS data i18n uc)
find_package(LibXml2 REQUIRED)
find_package(OpenSSL REQUIRED)
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
