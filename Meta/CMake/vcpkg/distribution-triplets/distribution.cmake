set(VCPKG_BUILD_TYPE release)
set(VCPKG_LIBRARY_LINKAGE static)

block(SCOPE_FOR VARIABLES PROPAGATE VCPKG_LIBRARY_LINKAGE VCPKG_HASH_ADDITIONAL_FILES)
    set(dynamic_ports_file "${CMAKE_CURRENT_LIST_DIR}/../dynamic-ports.txt")
    file(STRINGS "${dynamic_ports_file}" dynamic_ports REGEX "^[^#]")
    if (PORT IN_LIST dynamic_ports)
        set(VCPKG_LIBRARY_LINKAGE dynamic)
        # vcpkg hashes only the triplet file itself, so a cached static build would otherwise be reused.
        list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${dynamic_ports_file}")
    endif()
endblock()
