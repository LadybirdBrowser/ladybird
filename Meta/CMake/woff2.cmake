find_package(PkgConfig)
pkg_check_modules(WOFF2 REQUIRED IMPORTED_TARGET libwoff2dec)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    WOFF2
    REQUIRED_VARS
        WOFF2_INCLUDE_DIRS
        WOFF2_LIBRARY_DIRS
        WOFF2_LIBRARIES
)
