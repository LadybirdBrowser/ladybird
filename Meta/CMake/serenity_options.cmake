#
# Options specific to the Serenity (target) build
#

include(${CMAKE_CURRENT_LIST_DIR}/common_options.cmake NO_POLICY_SCOPE)

serenity_option(ENABLE_PCI_IDS_DOWNLOAD ON CACHE BOOL "Enable download of the pci.ids database at build time")
serenity_option(ENABLE_USB_IDS_DOWNLOAD ON CACHE BOOL "Enable download of the usb.ids database at build time")
serenity_option(ENABLE_PNP_IDS_DOWNLOAD ON CACHE BOOL "Enable download of the pnp.ids database at build time")
serenity_option(ENABLE_MOLD_LINKER OFF CACHE BOOL "Link the SerenityOS userland with the mold linker")
serenity_option(ENABLE_USERSPACE_COVERAGE_COLLECTION OFF CACHE BOOL "Enable code coverage instrumentation for userspace binaries in clang")
