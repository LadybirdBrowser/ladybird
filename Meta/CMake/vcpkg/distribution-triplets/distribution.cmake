set(VCPKG_BUILD_TYPE release)
set(VCPKG_LIBRARY_LINKAGE static)

block(SCOPE_FOR VARIABLES PROPAGATE VCPKG_LIBRARY_LINKAGE VCPKG_HASH_ADDITIONAL_FILES)
    set(dynamic_library_files
        "${CMAKE_CURRENT_LIST_DIR}/../../lgpl-libraries.txt"
        "${CMAKE_CURRENT_LIST_DIR}/../../dynamic-libraries.txt"
    )
    set(dynamic_ports "")
    foreach (dynamic_library_file IN LISTS dynamic_library_files)
        file(STRINGS "${dynamic_library_file}" libraries REGEX "^[^#]")
        list(APPEND dynamic_ports ${libraries})
    endforeach()
    if (PORT IN_LIST dynamic_ports)
        set(VCPKG_LIBRARY_LINKAGE dynamic)
        # vcpkg hashes only the triplet file itself, so a cached static build would otherwise be reused.
        list(APPEND VCPKG_HASH_ADDITIONAL_FILES ${dynamic_library_files})
    endif()
endblock()

if (VCPKG_CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PORT STREQUAL "qtbase")
    # vcpkg disables the AT-SPI bridge. Enable it for the Linux Qt runtime using the system AT-SPI headers.
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        -DCMAKE_DISABLE_FIND_PACKAGE_ATSPI2=OFF
        -DFEATURE_accessibility_atspi_bridge=ON
    )
endif()
