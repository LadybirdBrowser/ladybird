include_guard()

include(GNUInstallDirs) # make sure to include before we mess w/RPATH

# Handle multi-config generators (e.g. MSVC, Xcode, Ninja Multi-Config)
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(IN_BUILD_PREFIX "")
if (is_multi_config)
    set(IN_BUILD_PREFIX "$<CONFIG>/")
endif()

# Mirror the structure of the installed tree to ensure that rpaths
# always remain valid.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_BINDIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_LIBDIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_LIBDIR}")

# FIXME: Stop setting this when we have a good way to retrieve the directory that has the swift module
#        file for use by the swift frontend's header generator
set(CMAKE_Swift_MODULE_DIRECTORY  "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}swift")

set(CMAKE_SKIP_BUILD_RPATH FALSE)

if ("${VCPKG_INSTALLED_DIR}" STREQUAL "")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
else()
    if (CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
        set(CMAKE_BUILD_RPATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    else()
        set(CMAKE_BUILD_RPATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib")
    endif()
endif()

# See slide 100 of the following ppt :^)
# https://crascit.com/wp-content/uploads/2019/09/Deep-CMake-For-Library-Authors-Craig-Scott-CppCon-2019.pdf
if (APPLE)
    set(CMAKE_MACOSX_RPATH TRUE)
    set(CMAKE_INSTALL_NAME_DIR "@rpath")
    set(CMAKE_INSTALL_RPATH "@executable_path/../lib")
else()
    set(CMAKE_INSTALL_RPATH "$ORIGIN:$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(CMAKE_INSTALL_MESSAGE NEVER)
