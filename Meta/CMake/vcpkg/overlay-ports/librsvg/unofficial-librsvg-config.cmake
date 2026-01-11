file(READ "${CMAKE_CURRENT_LIST_DIR}/../librsvg/usage" usage)
message(WARNING "find_package(unofficial-librsvg) is deprecated.\n${usage}")

include(CMakeFindDependencyMacro)
find_dependency(PkgConfig)
pkg_check_modules(VCPKG_LIBRSVG librsvg-2.0 IMPORTED_TARGET)
if(NOT VCPKG_LIBRSVG_FOUND)
    set(${CMAKE_FIND_PACKAGE_NAME}_FOUND 0)
elseif(NOT TARGET unofficial::librsvg::rsvg-2)
    add_library(unofficial::librsvg::rsvg-2 INTERFACE IMPORTED)
    set_target_properties(unofficial::librsvg::rsvg-2 PROPERTIES
        INTERFACE_LINK_LIBRARIES PkgConfig::VCPKG_LIBRSVG
    )
endif()
