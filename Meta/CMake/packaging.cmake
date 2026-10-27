set(CPACK_GENERATOR "TGZ")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL js wasm)
set(CPACK_THREADS 0)
if (APPLE)
    if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(CPACK_SYSTEM_NAME "macOS-universal2")
    else()
        set(CPACK_SYSTEM_NAME "macOS-${CMAKE_SYSTEM_PROCESSOR}")
    endif()
else()
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CPACK_ARCHIVE_JS_FILE_NAME "ladybird-js-${CPACK_SYSTEM_NAME}")
set(CPACK_ARCHIVE_WASM_FILE_NAME "ladybird-wasm-${CPACK_SYSTEM_NAME}")

string(JOIN "-" ladybird_package_name ${LADYBIRD_VERSION} ${CPACK_SYSTEM_NAME})
set(CPACK_PACKAGE_FILE_NAME "ladybird-${ladybird_package_name}")
set(CPACK_ARCHIVE_LADYBIRD_RUNTIME_FILE_NAME "ladybird-${ladybird_package_name}")
set(CPACK_ARCHIVE_LADYBIRD_SYMBOLS_FILE_NAME "ladybird-symbols-${ladybird_package_name}")

set(CPACK_PACKAGE_VERSION "${LADYBIRD_PACKAGE_VERSION}")
set(CPACK_PACKAGE_VENDOR "Ladybird Browser Initiative")
set(CPACK_PACKAGE_CONTACT "Ladybird Browser Initiative <contact@ladybird.org>")
set(CPACK_PACKAGE_DESCRIPTION
    "Ladybird is a truly independent web browser, using a novel engine based on web standards.")
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/cpack_project_config.cmake")
set(CPACK_LADYBIRD_PACKAGE_INSTALL_PREFIX "${LADYBIRD_PACKAGE_INSTALL_PREFIX}")

# The Linux packages are the browser itself and its debug symbols, named like the debug packages of Debian and Fedora.
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_COMPRESSION_TYPE xz)
set(CPACK_DEBIAN_PACKAGE_SECTION web)
set(CPACK_DEBIAN_BROWSER_PACKAGE_NAME ladybird)
set(CPACK_DEBIAN_BROWSER_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_DEBIAN_BROWSER_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_DEBUGSYMBOLS_PACKAGE_NAME ladybird-dbgsym)
set(CPACK_DEBIAN_DEBUGSYMBOLS_PACKAGE_SECTION debug)
set(CPACK_DEBIAN_DEBUGSYMBOLS_PACKAGE_DEPENDS "ladybird (= ${LADYBIRD_PACKAGE_VERSION})")
set(CPACK_DEBIAN_DEBUGSYMBOLS_DESCRIPTION "Debug symbols for Ladybird, which debuggers find by build ID.")

set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "BSD-2-Clause")
set(CPACK_RPM_BROWSER_PACKAGE_NAME ladybird)
set(CPACK_RPM_BROWSER_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_DEBUGSYMBOLS_PACKAGE_NAME ladybird-debuginfo)
set(CPACK_RPM_DEBUGSYMBOLS_PACKAGE_REQUIRES "ladybird = ${LADYBIRD_PACKAGE_VERSION}")
set(CPACK_RPM_DEBUGSYMBOLS_PACKAGE_AUTOREQPROV OFF)
set(CPACK_RPM_DEBUGSYMBOLS_PACKAGE_SUMMARY "Debug symbols for Ladybird")
set(CPACK_RPM_DEBUGSYMBOLS_PACKAGE_DESCRIPTION "Debug symbols for Ladybird, which debuggers find by build ID.")

# The binaries are stripped and their debug symbols laid out by CPack already, so keep rpmbuild from doing it again.
# rpmbuild compresses on a single thread by default, which takes many minutes for packages of our size.
set(CPACK_RPM_SPEC_INSTALL_POST "/bin/true")
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none\n%define _binary_payload w9T0.xzdio")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /opt /usr/lib/debug /usr/lib/debug/.build-id /usr/share/applications
    /usr/share/dbus-1 /usr/share/dbus-1/services /usr/share/icons /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps /usr/share/metainfo)

include(CPack)
