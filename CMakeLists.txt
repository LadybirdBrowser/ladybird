cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/Meta/CMake")

project(SerenityOS C CXX ASM)

if(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "")
  message(FATAL_ERROR
    ": Don't use CMAKE_BUILD_TYPE when building serenity.\n"
    "The default build type is optimized with debug info and asserts enabled,\n"
    "and that's all there is.")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "SerenityOS")
    message(FATAL_ERROR "System name is not SerenityOS, this is unsupported.\n"
            "Please re-read the BuildInstructions documentation, and use the superbuild configuration\n")
endif()

# Check for toolchain mismatch, user might need to rebuild toolchain
set(GCC_VERSION "13.2.0")
set(LLVM_VERSION "18.1.3")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(EXPECTED_COMPILER_VERSION "${GCC_VERSION}")
else()
    set(EXPECTED_COMPILER_VERSION "${LLVM_VERSION}")
endif()
if (NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "${EXPECTED_COMPILER_VERSION}")
    message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} version (${CMAKE_CXX_COMPILER_VERSION}) does not match "
            "expected compiler version (${EXPECTED_COMPILER_VERSION}).\n"
            "Please rebuild the ${CMAKE_CXX_COMPILER_ID} Toolchain\n")
endif()

set(CMAKE_INSTALL_MESSAGE NEVER)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT COMMAND serenity_option)
    macro(serenity_option)
        set(${ARGV})
    endmacro()
endif()
include(serenity_options NO_POLICY_SCOPE)

include(setup_ccache)

if (NOT HACKSTUDIO_BUILD)

    include(check_for_dependencies)

    # Host tools, required to generate files for the build
    find_package(Lagom CONFIG REQUIRED)

    if (ENABLE_JAKT)
        find_package(Jakt CONFIG REQUIRED
            HINTS "${Lagom_DIR}/.."
        )
    endif()

endif()

# Meta target to run all code-gen steps in the build.
add_custom_target(all_generated)

add_custom_target(run
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SerenityOS_SOURCE_DIR}" "SERENITY_ARCH=${SERENITY_ARCH}" "${SerenityOS_SOURCE_DIR}/Meta/run.sh"
    USES_TERMINAL
)

# This can currently only be implemented by ordered commands
# as cmake doesn't support inter dependency ordering, and we
# would like to avoid inject dependencies on the existing
# custom commands to allow people to run commands adhoc with
# out forcing re-builds when they might not want them.
add_custom_target(setup-and-run
    COMMAND ${CMAKE_MAKE_PROGRAM} install
    COMMAND ${CMAKE_MAKE_PROGRAM} qemu-image
    COMMAND ${CMAKE_MAKE_PROGRAM} run
    USES_TERMINAL
)

add_custom_target(qemu-image
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SerenityOS_SOURCE_DIR}" "SERENITY_ARCH=${SERENITY_ARCH}" "SERENITY_TOOLCHAIN=${CMAKE_CXX_COMPILER_ID}" "LLVM_VERSION=${CMAKE_CXX_COMPILER_VERSION}" "${SerenityOS_SOURCE_DIR}/Meta/build-image-qemu.sh"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/_disk_image"
    USES_TERMINAL
)
add_custom_target(grub-image
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SerenityOS_SOURCE_DIR}" "SERENITY_ARCH=${SERENITY_ARCH}" "SERENITY_TOOLCHAIN=${CMAKE_CXX_COMPILER_ID}" "LLVM_VERSION=${CMAKE_CXX_COMPILER_VERSION}" "${SerenityOS_SOURCE_DIR}/Meta/build-image-grub.sh"
    BYPRODUCTS ${CMAKE_BINARY_DIR}/grub_disk_image
    USES_TERMINAL
)
add_custom_target(limine-image
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SerenityOS_SOURCE_DIR}" "SERENITY_ARCH=${SERENITY_ARCH}" "SERENITY_TOOLCHAIN=${CMAKE_CXX_COMPILER_ID}" "LLVM_VERSION=${CMAKE_CXX_COMPILER_VERSION}" "${SerenityOS_SOURCE_DIR}/Meta/build-image-limine.sh"
    BYPRODUCTS ${CMAKE_BINARY_DIR}/limine_disk_image
    USES_TERMINAL
)
add_custom_target(extlinux-image
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SerenityOS_SOURCE_DIR}" "SERENITY_ARCH=${SERENITY_ARCH}" "SERENITY_TOOLCHAIN=${CMAKE_CXX_COMPILER_ID}" "LLVM_VERSION=${CMAKE_CXX_COMPILER_VERSION}" "${SerenityOS_SOURCE_DIR}/Meta/build-image-extlinux.sh"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/extlinux_disk_image"
    USES_TERMINAL
)
add_custom_target(install-native-partition
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SerenityOS_SOURCE_DIR}" "SERENITY_ARCH=${SERENITY_ARCH}" "SERENITY_TOOLCHAIN=${CMAKE_CXX_COMPILER_ID}" "LLVM_VERSION=${CMAKE_CXX_COMPILER_VERSION}" "${SerenityOS_SOURCE_DIR}/Meta/build-native-partition.sh"
    USES_TERMINAL
)

add_custom_target(lint-shell-scripts
    COMMAND "${SerenityOS_SOURCE_DIR}/Meta/lint-shell-scripts.sh"
    USES_TERMINAL
)
add_custom_target(check-style
    COMMAND "${SerenityOS_SOURCE_DIR}/Meta/check-style.sh"
    USES_TERMINAL
)

add_custom_target(install-ports
    COMMAND "${CMAKE_COMMAND}" -E env "SERENITY_SOURCE_DIR=${SerenityOS_SOURCE_DIR}" "SERENITY_ARCH=${SERENITY_ARCH}" "SERENITY_TOOLCHAIN=${CMAKE_CXX_COMPILER_ID}" "${SerenityOS_SOURCE_DIR}/Meta/install-ports-tree.sh"
    USES_TERMINAL
)

if (NOT HACKSTUDIO_BUILD)

    add_custom_target(configure-components
        COMMAND "$<TARGET_FILE:Lagom::ConfigureComponents>"
        USES_TERMINAL
    )
    add_dependencies(configure-components Lagom::ConfigureComponents)

endif()

if (ENABLE_ALL_DEBUG_FACILITIES)
    set(ENABLE_ALL_THE_DEBUG_MACROS ON)

    # Immediately finds violations during boot, shouldn't be discoverable
    # by people who aren't working on fixing issues. Use this check to make
    # sure this code continues to build instead of all_debug_macros to avoid
    # people filing bugs.
    set(KMALLOC_VERIFY_NO_SPINLOCK_HELD ON)
endif()

if (ENABLE_ALL_THE_DEBUG_MACROS)
    include(all_the_debug_macros)
endif(ENABLE_ALL_THE_DEBUG_MACROS)

set(CMAKE_STAGING_PREFIX "${CMAKE_BINARY_DIR}/Root")
set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/Root")
set(CMAKE_INSTALL_DATAROOTDIR res)
set(CMAKE_INSTALL_INCLUDEDIR usr/include)
set(CMAKE_INSTALL_LIBDIR usr/lib)

configure_file(AK/Debug.h.in AK/Debug.h @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AK/Debug.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/AK")

# We disable it completely because it makes cmake very spammy.
# This will need to be revisited when the Loader supports RPATH/RUN_PATH.
set(CMAKE_SKIP_RPATH TRUE)

include(serenity_compile_options)

add_link_options(LINKER:-z,text)
add_link_options(LINKER:--no-allow-shlib-undefined)

add_compile_definitions(SANITIZE_PTRS)

if (ENABLE_COMPILETIME_FORMAT_CHECK)
    add_compile_definitions(ENABLE_COMPILETIME_FORMAT_CHECK)
endif()

if("${SERENITY_ARCH}" STREQUAL "aarch64")
    # FIXME: re-enable this warning
    add_compile_options(-Wno-type-limits)
endif()

add_link_options(-Wno-unused-command-line-argument)

include_directories(.)
include_directories(Userland/Libraries)
include_directories(Userland/Libraries/LibCrypt)
include_directories(Userland/Libraries/LibSystem)
include_directories(Userland/Services)
include_directories(Userland)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/Userland/Services)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/Userland/Libraries)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/Userland)

add_custom_target(components ALL)
option(BUILD_EVERYTHING "Build all optional components" ON)

include(utils)
include(flac_spec_tests)

serenity_component(
    Tests
    RECOMMENDED
)

if (HACKSTUDIO_BUILD)
    include_directories(/usr/include/Userland/Services)
    include(${HACKSTUDIO_BUILD_CMAKE_FILE})
    return()
endif()

add_library(GenericClangPlugin INTERFACE)
add_library(JSClangPlugin INTERFACE)
if (ENABLE_CLANG_PLUGINS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
    target_link_libraries(GenericClangPlugin INTERFACE Lagom::GenericClangPlugin)
    target_link_libraries(JSClangPlugin INTERFACE Lagom::JSClangPlugin)
endif()

add_subdirectory(AK)

# FIXME: vptr sanitizing requires.. intense ABI wrangling of std::type_info
#        And would be better served by porting ubsan_type_hash_itanium.cpp from compiler-rt
if (ENABLE_UNDEFINED_SANITIZER)
    add_compile_options(-fsanitize=undefined -fno-sanitize=vptr)
    add_link_options(-fsanitize=undefined -fno-sanitize=vptr)

    if (UNDEFINED_BEHAVIOR_IS_FATAL)
        add_compile_options(-fno-sanitize-recover=undefined)
    endif()
endif()

if (ENABLE_MOLD_LINKER)
    add_link_options(-fuse-ld=mold)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$" OR ENABLE_MOLD_LINKER)
    add_link_options(LINKER:--pack-dyn-relocs=relr)
elseif("${SERENITY_ARCH}" STREQUAL "x86_64")
    # The BFD linker only supports RELR relocations on x86 and POWER.
    add_link_options(LINKER:-z,pack-relative-relocs)
endif()

add_subdirectory(Userland)
add_subdirectory(Tests)

if (ENABLE_COMPILETIME_HEADER_CHECK)
    add_subdirectory(Meta/HeaderCheck)
endif()

export_components("${CMAKE_BINARY_DIR}/components.ini")

# it seems like cmake cannot uncompress *.gz files... only ZIP/tar.gz.
# Lets download the uncompressed versions.
# https://gitlab.kitware.com/cmake/cmake/-/issues/23054

set(PCI_IDS_FILE pci.ids)
set(PCI_IDS_URL "https://pci-ids.ucw.cz/v2.2/${PCI_IDS_FILE}")
set(PCI_IDS_DOWNLOAD_PATH "${SERENITY_CACHE_DIR}/${PCI_IDS_FILE}")
set(PCI_IDS_INSTALL_PATH "${CMAKE_STAGING_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}")

if (ENABLE_PCI_IDS_DOWNLOAD AND NOT EXISTS "${PCI_IDS_INSTALL_PATH}/${PCI_IDS_FILE}")
    download_file("${PCI_IDS_URL}" "${PCI_IDS_DOWNLOAD_PATH}")
    install(FILES "${PCI_IDS_DOWNLOAD_PATH}" DESTINATION "${PCI_IDS_INSTALL_PATH}")
endif()

set(USB_IDS_FILE usb.ids)
set(USB_IDS_URL "http://www.linux-usb.org/${USB_IDS_FILE}")
set(USB_IDS_DOWNLOAD_PATH "${SERENITY_CACHE_DIR}/${USB_IDS_FILE}")
set(USB_IDS_INSTALL_PATH "${CMAKE_STAGING_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}")

if (ENABLE_USB_IDS_DOWNLOAD AND NOT EXISTS "${USB_IDS_INSTALL_PATH}/${USB_IDS_FILE}")
    download_file("${USB_IDS_URL}" "${USB_IDS_DOWNLOAD_PATH}")
    install(FILES "${USB_IDS_DOWNLOAD_PATH}" DESTINATION "${USB_IDS_INSTALL_PATH}")
endif()
