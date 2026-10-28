# The archives keep a relocatable layout of Ladybird's components, while the Linux packages install into a fixed
# directory and group the components into the browser package and its debug symbols package.
if (CPACK_GENERATOR MATCHES "^(DEB|RPM)$")
    set(CPACK_PACKAGING_INSTALL_PREFIX "${CPACK_LADYBIRD_PACKAGE_INSTALL_PREFIX}")
    set(CPACK_PACKAGE_RELOCATABLE OFF)
    set(CPACK_COMPONENT_LADYBIRD_RUNTIME_GROUP Browser)
    set(CPACK_COMPONENT_LADYBIRD_SYSTEMINTEGRATION_GROUP Browser)
    set(CPACK_COMPONENT_LADYBIRD_SYSTEMSYMBOLS_GROUP DebugSymbols)
endif()
