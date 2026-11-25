
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(package Ladybird)

if (APPLE)
    set(ladybird_executable_install_dir bundle/Ladybird.app/Contents/MacOS)
    set(ladybird_helper_install_dir ${ladybird_executable_install_dir})
else()
    set(ladybird_executable_install_dir ${CMAKE_INSTALL_BINDIR})
    set(ladybird_helper_install_dir ${CMAKE_INSTALL_LIBEXECDIR})
endif()

set(ladybird_applications ladybird WebDriver ${ladybird_helper_processes})

# Builds with vcpkg ship the shared libraries it built, which are found by what the installed binaries link.
include(install_vcpkg_libraries)
set(vcpkg_runtime_dependencies "")
if (LADYBIRD_INSTALLS_VCPKG_LIBRARIES)
    set(vcpkg_runtime_dependencies RUNTIME_DEPENDENCY_SET ladybird_vcpkg_libraries)
endif()

# CMake installs the binaries unstripped when it has no strip tool, or ignores the failure to run it, for instance when
# packaging in a different environment than the build. Refuse to install anything in that case. MSVC-style toolchains
# have no strip tool to begin with.
if (NOT MSVC)
    install(CODE "
    if (CMAKE_INSTALL_DO_STRIP AND NOT EXISTS \"${CMAKE_STRIP}\")
      message(FATAL_ERROR \"Cannot strip the installed binaries, since the strip tool '${CMAKE_STRIP}' does not exist\")
    endif()
  "
            COMPONENT ladybird_Runtime)
endif()

install(TARGETS ladybird WebDriver
  EXPORT ladybirdTargets
  ${vcpkg_runtime_dependencies}
  RUNTIME
    COMPONENT ladybird_Runtime
    DESTINATION ${ladybird_executable_install_dir}
  BUNDLE
    COMPONENT ladybird_Runtime
    DESTINATION bundle
  LIBRARY
    COMPONENT ladybird_Runtime
    NAMELINK_COMPONENT ladybird_Development
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  FILE_SET browser
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILE_SET ladybird
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS ${ladybird_helper_processes}
  EXPORT ladybirdTargets
  ${vcpkg_runtime_dependencies}
  RUNTIME
    COMPONENT ladybird_Runtime
    DESTINATION ${ladybird_helper_install_dir}
)

include("${LADYBIRD_SOURCE_DIR}/Meta/CMake/get_linked_lagom_libraries.cmake")
foreach (application IN LISTS ladybird_applications)
  get_linked_lagom_libraries("${application}" "${application}_lagom_libraries")
  list(APPEND all_required_lagom_libraries "${${application}_lagom_libraries}")
endforeach()
list(REMOVE_DUPLICATES all_required_lagom_libraries)

# Remove ladybird shlib if it exists
list(REMOVE_ITEM all_required_lagom_libraries ladybird)

if (APPLE)
    # Fixup the app bundle and copy:
    #   - Shared libraries from lib/ to Ladybird.app/Contents/lib
    # Static libraries are linked into the executables, so nothing loads them from the bundle.
    # CMake strips every executable it installs except the one inside an application bundle, so strip that one here.
    # Remove the symlink we created at build time for the lib directory first
    install(CODE "
    set(contents_dir \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bundle/Ladybird.app/Contents)
    file(REMOVE \${contents_dir}/lib)
    set(lib_dir \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR})
    if (IS_ABSOLUTE ${CMAKE_INSTALL_LIBDIR})
      set(lib_dir \$ENV{DESTDIR}${CMAKE_INSTALL_LIBDIR})
    endif()

    if (CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND \"${CMAKE_STRIP}\" -u -r \${contents_dir}/MacOS/$<TARGET_FILE_NAME:ladybird>
        COMMAND_ERROR_IS_FATAL ANY)
    endif()

    file(GLOB shared_libraries \${lib_dir}/*.dylib)
    if (shared_libraries)
      file(COPY \${shared_libraries} DESTINATION \${contents_dir}/lib)
    endif()
  "
            COMPONENT ladybird_Runtime)

    install(FILES "${LADYBIRD_SOURCE_DIR}/Meta/Entitlements.plist" DESTINATION entitlements
        RENAME Ladybird.release.entitlements COMPONENT ladybird_Runtime)
endif()

install(TARGETS ${all_required_lagom_libraries}
  EXPORT ladybirdTargets
  ${vcpkg_runtime_dependencies}
  COMPONENT ladybird_Runtime
  LIBRARY
    COMPONENT ladybird_Runtime
    NAMELINK_COMPONENT ladybird_Development
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  FILE_SET server
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILE_SET ladybird
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if (vcpkg_runtime_dependencies)
    set(vcpkg_library_install_dir ${CMAKE_INSTALL_LIBDIR})
    set(qt_plugins "")
    if (APPLE)
        set(vcpkg_library_install_dir bundle/Ladybird.app/Contents/lib)
        set(qt_plugins_dir bundle/Ladybird.app/Contents/PlugIns)
        set(qt_conf_dir bundle/Ladybird.app/Contents/Resources)
        set(qt_conf_plugins PlugIns)

        get_target_property(qt_core_type Qt6::Core TYPE)
        if (qt_core_type STREQUAL "SHARED_LIBRARY")
            set(qt_plugins
                imageformats/QGifPlugin
                imageformats/QICOPlugin
                imageformats/QJpegPlugin
                platforms/QCocoaIntegrationPlugin
                styles/QMacStylePlugin
            )
        endif()
    elseif (LINUX AND "qt" IN_LIST VCPKG_MANIFEST_FEATURES)
        # Keep the vcpkg plugin layout, whose relative runtime paths point to lib/.
        set(qt_plugins_dir Qt6/plugins)

        # Add the installed plugin directory at startup without overriding Qt for other applications.
        cmake_path(ABSOLUTE_PATH ladybird_executable_install_dir BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
            NORMALIZE OUTPUT_VARIABLE qt_executable_dir)
        cmake_path(ABSOLUTE_PATH qt_plugins_dir BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
            NORMALIZE OUTPUT_VARIABLE qt_plugins_path)
        cmake_path(RELATIVE_PATH qt_plugins_path BASE_DIRECTORY "${qt_executable_dir}")
        target_compile_definitions(ladybird PRIVATE "LADYBIRD_QT_PLUGINS_PATH=\"${qt_plugins_path}\"")

        set(qt_plugins
            iconengines/QSvgIconPlugin
            imageformats/QGifPlugin
            imageformats/QICOPlugin
            imageformats/QJpegPlugin
            imageformats/QSvgPlugin
            platforminputcontexts/QComposePlatformInputContextPlugin
            platforminputcontexts/QIbusPlatformInputContextPlugin
            platforms/QOffscreenIntegrationPlugin
            platforms/QWaylandIntegrationPlugin
            platforms/QXcbIntegrationPlugin
            platformthemes/QXdgDesktopPortalThemePlugin
            position/QGeoPositionInfoSourceFactoryGeoclue2Plugin
            position/QGeoPositionInfoSourceFactoryNmeaPlugin
            wayland-decoration-client/QWaylandBradientDecorationPlugin
            wayland-graphics-integration-client/QWaylandEglClientBufferPlugin
            wayland-shell-integration/QWaylandXdgShellIntegrationPlugin
            xcbglintegrations/QXcbEglIntegrationPlugin
            xcbglintegrations/QXcbGlxIntegrationPlugin
        )
        install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/sbom/"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ladybird/qt
            COMPONENT ladybird_Runtime
            FILES_MATCHING PATTERN "qt*.spdx"
        )
    endif()

    # Qt loads these at runtime. Add them to the dependency set to collect their shared libraries as well.
    if (qt_plugins)
        foreach (plugin IN LISTS qt_plugins)
            cmake_path(GET plugin PARENT_PATH plugin_type)
            cmake_path(GET plugin FILENAME plugin_target)
            if (NOT TARGET Qt6::${plugin_target})
                message(FATAL_ERROR "Ladybird needs the Qt plugin ${plugin_target}, which Qt was built without.")
            endif()
            set(plugin_install_dir "${qt_plugins_dir}/${plugin_type}")
            install(IMPORTED_RUNTIME_ARTIFACTS Qt6::${plugin_target}
                RUNTIME_DEPENDENCY_SET ladybird_vcpkg_libraries
                LIBRARY DESTINATION ${plugin_install_dir} COMPONENT ladybird_Runtime
            )
            if (LINUX AND NOT vcpkg_library_install_dir STREQUAL "lib")
                # Make the imported plugin's runpath relative to the actual library destination.
                find_program(PATCHELF_EXECUTABLE patchelf REQUIRED)
                install(CODE "
                    set(qt_library_dir \"${vcpkg_library_install_dir}\")
                    set(qt_plugin_dir \"\${CMAKE_INSTALL_PREFIX}/${plugin_install_dir}\")
                    cmake_path(ABSOLUTE_PATH qt_library_dir BASE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}\" NORMALIZE)
                    cmake_path(RELATIVE_PATH qt_library_dir BASE_DIRECTORY \"\${qt_plugin_dir}\")
                    execute_process(COMMAND \"${PATCHELF_EXECUTABLE}\" --set-rpath \"\$ORIGIN:\$ORIGIN/\${qt_library_dir}\"
                        \"\$ENV{DESTDIR}\${qt_plugin_dir}/$<TARGET_FILE_NAME:Qt6::${plugin_target}>\"
                        COMMAND_ERROR_IS_FATAL ANY)
                " COMPONENT ladybird_Runtime)
            endif()
        endforeach()

        if (APPLE)
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "[Paths]\nPlugins = ${qt_conf_plugins}\n")
            install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" DESTINATION ${qt_conf_dir} COMPONENT ladybird_Runtime)
        endif()
    endif()

    # LibMediaCodecs loads the bundled FFmpeg at runtime on Linux. Add it to the dependency set to collect the libraries
    # it depends on as well.
    if (TARGET BundledFFmpeg::AVCODEC)
        install(IMPORTED_RUNTIME_ARTIFACTS BundledFFmpeg::AVCODEC BundledFFmpeg::LIBSWRESAMPLE
            RUNTIME_DEPENDENCY_SET ladybird_vcpkg_libraries
            LIBRARY DESTINATION ${vcpkg_library_install_dir} COMPONENT ladybird_Runtime
        )
    endif()

    ladybird_install_vcpkg_libraries(ladybird_vcpkg_libraries
        DESTINATION ${vcpkg_library_install_dir} COMPONENT ladybird_Runtime)
endif()

if (LADYBIRD_GENERATE_SYMBOL_FILES)
    set(symbol_files_dir "${CMAKE_BINARY_DIR}/symbols")
    set(symbol_files)
    foreach (target IN LISTS ladybird_applications)
        get_target_property(name ${target} OUTPUT_NAME)
        if (NOT name)
            set(name ${target})
        endif()
        if (APPLE)
            get_target_property(is_bundle ${target} MACOSX_BUNDLE)
            if (is_bundle)
                string(APPEND name ".app")
            endif()
            set(symbol_file "${symbol_files_dir}/${name}.dSYM")
            set(output "${symbol_file}/Contents/Info.plist")
            set(command dsymutil "$<TARGET_FILE:${target}>" -o "${symbol_file}")
        else()
            set(output "${CMAKE_BINARY_DIR}/symbol_file_stamps/${name}")
            set(command "${CMAKE_COMMAND}" "-DREADELF=${CMAKE_READELF}" "-DOBJCOPY=${CMAKE_OBJCOPY}"
                "-DBINARY=$<TARGET_FILE:${target}>" "-DSYMBOL_FILES_DIR=${symbol_files_dir}" "-DSTAMP=${output}"
                -P "${LADYBIRD_SOURCE_DIR}/Meta/CMake/write_symbol_file.cmake")
        endif()
        add_custom_command(OUTPUT "${output}" COMMAND ${command} DEPENDS ${target}
            COMMENT "Writing the symbols of ${name}" VERBATIM)
        list(APPEND symbol_files "${output}")
    endforeach()
    add_custom_target(ladybird_symbol_files ALL DEPENDS ${symbol_files})

    install(DIRECTORY "${symbol_files_dir}/"
        DESTINATION symbols
        COMPONENT ladybird_Symbols
        EXCLUDE_FROM_ALL
    )

    if (LINUX)
        install(DIRECTORY "${symbol_files_dir}/.build-id"
            DESTINATION /usr/lib/debug
            COMPONENT ladybird_SystemSymbols
            EXCLUDE_FROM_ALL
        )
    endif()
endif()

write_basic_package_version_file(
    "${package}ConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
)

# Allow package maintainers to freely override the path for the configs
set(
    ladybird_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/${package}"
    CACHE PATH "CMake package config location relative to the install prefix"
)
mark_as_advanced(ladybird_INSTALL_CMAKEDIR)

install(
    FILES cmake/LadybirdInstallConfig.cmake
    DESTINATION "${ladybird_INSTALL_CMAKEDIR}"
    RENAME "${package}Config.cmake"
    COMPONENT ladybird_Development
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/${package}ConfigVersion.cmake"
    DESTINATION "${ladybird_INSTALL_CMAKEDIR}"
    COMPONENT ladybird_Development
)

install(
    EXPORT ladybirdTargets
    NAMESPACE ladybird::
    DESTINATION "${ladybird_INSTALL_CMAKEDIR}"
    COMPONENT ladybird_Development
)

if (NOT APPLE)
    # On macOS the resources are handled via the MACOSX_PACKAGE_LOCATION property on each resource file
    install_ladybird_resources("${CMAKE_INSTALL_DATADIR}/Lagom" ladybird_Runtime)
endif()

if (ENABLE_INSTALL_FREEDESKTOP_FILES OR LINUX)
    set(FREEDESKTOP_RESOURCE_DIR "${LADYBIRD_SOURCE_DIR}/Meta/CMake/freedesktop")
    string(TIMESTAMP DATE "%Y-%m-%d" UTC)

    # Source tarballs record their commit in a REVISION file, since they have no Git history of their own. They might be
    # unpacked inside another Git repository though, so the file takes precedence.
    if (EXISTS "${LADYBIRD_SOURCE_DIR}/REVISION")
        file(STRINGS "${LADYBIRD_SOURCE_DIR}/REVISION" GIT_HASH LIMIT_COUNT 1 REGEX "^[0-9a-f]+$")
        string(SUBSTRING "${GIT_HASH}" 0 10 GIT_HASH)
    else()
        execute_process(
            COMMAND git rev-parse --short=10 HEAD
            WORKING_DIRECTORY ${LADYBIRD_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()
    if (LADYBIRD_VERSION STREQUAL "")
        set(METAINFO_RELEASE_VERSION "${CMAKE_PROJECT_VERSION}-snapshot-${GIT_HASH}")
        set(METAINFO_RELEASE_TYPE "snapshot")
        set(METAINFO_RELEASE_DESCRIPTION "Development snapshot.")
    elseif (LADYBIRD_RELEASE_STAGE STREQUAL "")
        set(METAINFO_RELEASE_VERSION "${LADYBIRD_VERSION}")
        set(METAINFO_RELEASE_TYPE "stable")
        set(METAINFO_RELEASE_DESCRIPTION "Stable release.")
    else()
        set(METAINFO_RELEASE_VERSION "${LADYBIRD_VERSION}")
        set(METAINFO_RELEASE_TYPE "development")
        set(METAINFO_RELEASE_DESCRIPTION "${LADYBIRD_RELEASE_STAGE} release.")
    endif()
    configure_file("${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.metainfo.xml.in" "${CMAKE_CURRENT_BINARY_DIR}/org.ladybird.Ladybird.metainfo.xml" @ONLY)
endif()

# The archives and Linux packages leave these out: an archive can be unpacked anywhere, and the Linux packages carry
# copies in /usr that point at the installed command.
if (ENABLE_INSTALL_FREEDESKTOP_FILES)
    install(FILES
        "${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        COMPONENT ladybird_Freedesktop
    )
    install(FILES
        "${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
        COMPONENT ladybird_Freedesktop
    )
    install(FILES
        "${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.service"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/services"
        COMPONENT ladybird_Freedesktop
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/org.ladybird.Ladybird.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
        COMPONENT ladybird_Freedesktop
    )
endif()

# Linux packages install Ladybird into LADYBIRD_PACKAGE_INSTALL_PREFIX, and only add a command, the desktop integration
# and the debug symbols to the rest of the system.
if (LINUX)
    set(system_integration_dir "${CMAKE_CURRENT_BINARY_DIR}/system_integration")
    set(packaged_ladybird "${LADYBIRD_PACKAGE_INSTALL_PREFIX}/bin/Ladybird")
    foreach (file IN ITEMS org.ladybird.Ladybird.desktop org.ladybird.Ladybird.service)
        file(READ "${FREEDESKTOP_RESOURCE_DIR}/${file}" contents)
        string(REGEX REPLACE "Exec=Ladybird( |\n|$)" "Exec=${packaged_ladybird}\\1" contents "${contents}")
        file(WRITE "${system_integration_dir}/${file}" "${contents}")
    endforeach()
    file(MAKE_DIRECTORY "${system_integration_dir}/bin")
    file(CREATE_LINK "${packaged_ladybird}" "${system_integration_dir}/bin/ladybird" SYMBOLIC)

    install(FILES "${system_integration_dir}/bin/ladybird"
        DESTINATION /usr/bin
        COMPONENT ladybird_SystemIntegration
        EXCLUDE_FROM_ALL
    )
    install(FILES "${system_integration_dir}/org.ladybird.Ladybird.desktop"
        DESTINATION /usr/share/applications
        COMPONENT ladybird_SystemIntegration
        EXCLUDE_FROM_ALL
    )
    install(FILES "${system_integration_dir}/org.ladybird.Ladybird.service"
        DESTINATION /usr/share/dbus-1/services
        COMPONENT ladybird_SystemIntegration
        EXCLUDE_FROM_ALL
    )
    install(FILES "${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.svg"
        DESTINATION /usr/share/icons/hicolor/scalable/apps
        COMPONENT ladybird_SystemIntegration
        EXCLUDE_FROM_ALL
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.ladybird.Ladybird.metainfo.xml"
        DESTINATION /usr/share/metainfo
        COMPONENT ladybird_SystemIntegration
        EXCLUDE_FROM_ALL
    )
endif()
