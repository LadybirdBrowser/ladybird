file(READ "${CMAKE_CURRENT_LIST_DIR}/usage" usage)
message(AUTHOR_WARNING "find_package(skia) is deprecated.\n${usage}")
include(CMakeFindDependencyMacro)
find_dependency(unofficial-skia)
if(NOT TARGET skia)
    get_filename_component(z_vcpkg_skia_root "${CMAKE_CURRENT_LIST_FILE}" PATH)
    get_filename_component(z_vcpkg_skia_root "${z_vcpkg_skia_root}" PATH)
    get_filename_component(z_vcpkg_skia_root "${z_vcpkg_skia_root}" PATH)
    if(z_vcpkg_skia_root STREQUAL "/")
        set(z_vcpkg_skia_root "")
    endif()
    add_library(skia INTERFACE IMPORTED)
    set_target_properties(skia PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${z_vcpkg_skia_root}/include"
        INTERFACE_LINK_LIBRARIES unofficial::skia::skia
    )
    add_library(skia::skia ALIAS skia)
    unset(z_vcpkg_skia_root)
endif()
