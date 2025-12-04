# Windows Installer Configuration using CPack
# This module sets up NSIS-based installer generation for Windows

if(NOT WIN32)
    return()
endif()

# Only enable installer when explicitly requested
if(NOT ENABLE_INSTALLER)
    return()
endif()

include(InstallRequiredSystemLibraries)

# Set CPack variables
set(CPACK_PACKAGE_NAME "Ladybird")
set(CPACK_PACKAGE_VENDOR "Ladybird Browser Initiative")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Ladybird - A truly independent web browser")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Ladybird")

# Resource files
set(CPACK_RESOURCE_FILE_LICENSE "${LADYBIRD_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${LADYBIRD_SOURCE_DIR}/README.md")

# NSIS-specific settings
set(CPACK_GENERATOR "NSIS;ZIP")
set(CPACK_NSIS_DISPLAY_NAME "Ladybird Browser")
set(CPACK_NSIS_PACKAGE_NAME "Ladybird")
set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\Ladybird.exe")
set(CPACK_NSIS_HELP_LINK "https://ladybird.org")
set(CPACK_NSIS_URL_INFO_ABOUT "https://ladybird.org")
set(CPACK_NSIS_CONTACT "https://github.com/LadybirdBrowser/ladybird")
set(CPACK_NSIS_MODIFY_PATH ON)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

# Create desktop shortcut
set(CPACK_NSIS_CREATE_ICONS_EXTRA
    "CreateShortCut '$DESKTOP\\\\Ladybird.lnk' '$INSTDIR\\\\bin\\\\Ladybird.exe'"
)

# Remove desktop shortcut on uninstall
set(CPACK_NSIS_DELETE_ICONS_EXTRA
    "Delete '$DESKTOP\\\\Ladybird.lnk'"
)

# Start menu shortcuts
set(CPACK_NSIS_MENU_LINKS
    "bin/Ladybird.exe" "Ladybird Browser"
    "https://ladybird.org" "Ladybird Website"
)

# WiX-specific settings (alternative to NSIS)
set(CPACK_WIX_UPGRADE_GUID "8B5F3F3E-9C4D-4E5F-8A6B-7C8D9E0F1A2B")
set(CPACK_WIX_PRODUCT_GUID "*")
set(CPACK_WIX_PRODUCT_ICON "${LADYBIRD_SOURCE_DIR}/Base/res/icons/ladybird.ico")
set(CPACK_WIX_UI_BANNER "${LADYBIRD_SOURCE_DIR}/Meta/Installer/banner.bmp")
set(CPACK_WIX_UI_DIALOG "${LADYBIRD_SOURCE_DIR}/Meta/Installer/dialog.bmp")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Ladybird")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://ladybird.org")

# Component installation
set(CPACK_COMPONENTS_ALL ladybird_Runtime)
set(CPACK_COMPONENT_LADYBIRD_RUNTIME_DISPLAY_NAME "Ladybird Browser")
set(CPACK_COMPONENT_LADYBIRD_RUNTIME_DESCRIPTION "The Ladybird web browser application and required libraries")
set(CPACK_COMPONENT_LADYBIRD_RUNTIME_REQUIRED ON)

# Architecture-specific naming
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CPACK_PACKAGE_FILE_NAME "Ladybird-${CPACK_PACKAGE_VERSION}-win64")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
else()
    set(CPACK_PACKAGE_FILE_NAME "Ladybird-${CPACK_PACKAGE_VERSION}-win32")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES")
endif()

# Include CPack module
include(CPack)

message(STATUS "Windows Installer: Enabled")
message(STATUS "  Generators: ${CPACK_GENERATOR}")
message(STATUS "  Package Name: ${CPACK_PACKAGE_FILE_NAME}")
message(STATUS "  Install Directory: ${CPACK_PACKAGE_INSTALL_DIRECTORY}")
