# Exported from "@gn_target@"
if("@not_executable@")
    set_property(TARGET @cmake_target@ APPEND PROPERTY INTERFACE_LINK_LIBRARIES "\$<LINK_ONLY:\$<@cmake_config_genex@:@interface_link_targets@>>")
endif()
if("@has_location@")
    set_property(TARGET @cmake_target@ APPEND PROPERTY IMPORTED_CONFIGURATIONS "@cmake_build_type@")
    set_target_properties(@cmake_target@ PROPERTIES IMPORTED_LOCATION_@cmake_build_type@ "@imported_location@")
    if("@not_executable@")
        set_property(TARGET @cmake_target@ APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS "\$<@cmake_config_genex@:@interface_compile_definitions@>")
        set_target_properties(@cmake_target@ PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "@CURRENT_PACKAGES_DIR@/include/@PORT@"
            IMPORTED_LINK_INTERFACE_LANGUAGES_@cmake_build_type@ "@link_language@"
        )
        z_vcpkg_@PORT@_get_link_libraries(z_vcpkg_@PORT@_link_libs "@cmake_build_type@" "@interface_link_libs@")
        set_property(TARGET @cmake_target@ APPEND PROPERTY INTERFACE_LINK_LIBRARIES "\$<LINK_ONLY:\$<@cmake_config_genex@:${z_vcpkg_@PORT@_link_libs}>>")
        unset(z_vcpkg_@PORT@_link_libs)
    endif()
endif()
