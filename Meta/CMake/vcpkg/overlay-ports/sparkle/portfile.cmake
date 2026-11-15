# Sparkle is built by its Xcode project, so this port needs Xcode rather than only the Command Line Tools.
set(VCPKG_BUILD_TYPE release)
set(VCPKG_POLICY_DLLS_IN_STATIC_LIBRARY enabled)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
# The framework finds its own parts relative to itself, so its install names and rpaths stay as Xcode made them.
set(VCPKG_FIXUP_MACHO_RPATH OFF)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sparkle-project/Sparkle
    REF "${VERSION}"
    SHA512 2eb619b6a4791895041200c0b8f0973b68879df99d43a95528fc9c8f0e6ef483939b04483c321d342a12a7287b2398ad86f2c5df0a72f7599033fda837f7729e
    HEAD_REF 2.x
    PATCHES
        # Only Sparkle's command line tools use swift-argument-parser, and Xcode would fetch it while building.
        no-swift-packages.patch
)

if(DEFINED ENV{DEVELOPER_DIR})
    set(developer_dir "$ENV{DEVELOPER_DIR}")
else()
    execute_process(COMMAND xcode-select --print-path OUTPUT_VARIABLE developer_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT EXISTS "${developer_dir}/usr/bin/xcodebuild" AND EXISTS "/Applications/Xcode.app/Contents/Developer/usr/bin/xcodebuild")
    set(developer_dir "/Applications/Xcode.app/Contents/Developer")
endif()
if(NOT EXISTS "${developer_dir}/usr/bin/xcodebuild")
    message(FATAL_ERROR "Building Sparkle needs Xcode. Install it, or point DEVELOPER_DIR at its Developer directory.")
endif()

# Ladybird is not sandboxed, so it needs none of Sparkle's XPC services. It also needs none of the support for older
# updaters, DSA signatures, older delta updates or installer packages.
set(build_settings)
if(VCPKG_OSX_ARCHITECTURES)
    list(JOIN VCPKG_OSX_ARCHITECTURES " " archs)
    list(APPEND build_settings "ARCHS=${archs}")
endif()
if(VCPKG_OSX_DEPLOYMENT_TARGET)
    list(APPEND build_settings "MACOSX_DEPLOYMENT_TARGET=${VCPKG_OSX_DEPLOYMENT_TARGET}")
endif()

set(build_dir "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel")
file(REMOVE_RECURSE "${build_dir}")
vcpkg_execute_build_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DEVELOPER_DIR=${developer_dir}"
        xcodebuild build
            -project "${SOURCE_PATH}/Sparkle.xcodeproj"
            -target Sparkle
            -configuration Release
            ${build_settings}
            CODE_SIGNING_ALLOWED=NO
            "SYMROOT=${build_dir}"
            "OBJROOT=${build_dir}/obj"
            SPARKLE_EMBED_INSTALLER_LAUNCHER_XPC_SERVICE=0
            SPARKLE_EMBED_DOWNLOADER_XPC_SERVICE=0
            SPARKLE_BUILD_LEGACY_SUUPDATER=0
            SPARKLE_BUILD_LEGACY_DSA_SUPPORT=0
            SPARKLE_BUILD_LEGACY_DELTA_SUPPORT=0
            SPARKLE_BUILD_PACKAGE_SUPPORT=0
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME "build-${TARGET_TRIPLET}-rel"
)

file(COPY "${build_dir}/Release/Sparkle.framework" DESTINATION "${CURRENT_PACKAGES_DIR}/Frameworks")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/unofficial-sparkle-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/unofficial-sparkle")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
