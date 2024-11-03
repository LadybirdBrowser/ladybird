include_guard()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSEAUDIO IMPORTED_TARGET libpulse)

if (PULSEAUDIO_FOUND)
    set(HAVE_PULSEAUDIO ON CACHE BOOL "" FORCE)
endif()
