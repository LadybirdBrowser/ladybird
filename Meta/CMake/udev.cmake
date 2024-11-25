include_guard()

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV IMPORTED_TARGET libudev)

if (UDEV_FOUND)
    set(HAS_UDEV ON CACHE BOOL "" FORCE)
endif()
