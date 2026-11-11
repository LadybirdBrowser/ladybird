# The archives keep a relocatable layout of Ladybird's components, while the Linux packages install into a fixed
# directory and group the components into the browser package and its debug symbols package.
if (CPACK_GENERATOR MATCHES "^(DEB|RPM)$")
    set(CPACK_PACKAGING_INSTALL_PREFIX "${CPACK_LADYBIRD_PACKAGE_INSTALL_PREFIX}")
    set(CPACK_PACKAGE_RELOCATABLE OFF)
    set(CPACK_COMPONENT_LADYBIRD_RUNTIME_GROUP Browser)
    set(CPACK_COMPONENT_LADYBIRD_SYSTEMINTEGRATION_GROUP Browser)
    set(CPACK_COMPONENT_LADYBIRD_SYSTEMSYMBOLS_GROUP DebugSymbols)
endif()

# A Linux package links the Qt of the distribution version that built it, so its version names that distribution
# version: Debian packages like Ubuntu's backports do, RPM packages with the dist tag of rpmbuild. Builds for several
# versions of a distribution can then share a repository, and upgrading the distribution also moves Ladybird to the
# build for it.
if (CPACK_GENERATOR STREQUAL "DEB")
    file(STRINGS /etc/os-release os_release REGEX "^(ID|VERSION_ID)=")
    foreach (line IN LISTS os_release)
        if (line MATCHES "^(ID|VERSION_ID)=\"?([^\"]*)\"?$")
            set(os_release_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    set(CPACK_DEBIAN_PACKAGE_RELEASE "1~${os_release_ID}${os_release_VERSION_ID}")
    set(CPACK_DEBIAN_DEBUGSYMBOLS_PACKAGE_DEPENDS
        "ladybird (= ${CPACK_PACKAGE_VERSION}-${CPACK_DEBIAN_PACKAGE_RELEASE})")
endif()
