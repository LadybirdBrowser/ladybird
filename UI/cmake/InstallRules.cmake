
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

install(TARGETS ladybird WebDriver
  EXPORT ladybirdTargets
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
endif()

install(TARGETS ${all_required_lagom_libraries}
  EXPORT ladybirdTargets
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
            set(symbol_file "${symbol_files_dir}/${name}.debug")
            set(output "${symbol_file}")
            set(command "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:${target}>" "${symbol_file}")
        endif()
        add_custom_command(OUTPUT "${output}" COMMAND ${command} DEPENDS ${target}
            COMMENT "Writing the symbols of ${name}" VERBATIM)
        list(APPEND symbol_files "${output}")
    endforeach()
    add_custom_target(ladybird_symbol_files ALL DEPENDS ${symbol_files})

    install(DIRECTORY "${symbol_files_dir}/"
        DESTINATION .
        COMPONENT ladybird_Symbols
        EXCLUDE_FROM_ALL
    )
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

if (ENABLE_INSTALL_FREEDESKTOP_FILES)
    set(FREEDESKTOP_RESOURCE_DIR "${LADYBIRD_SOURCE_DIR}/Meta/CMake/freedesktop")
    string(TIMESTAMP DATE "%Y-%m-%d" UTC)
    execute_process(
        COMMAND git rev-parse --short=10 HEAD
        WORKING_DIRECTORY ${LADYBIRD_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    configure_file("${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.metainfo.xml.in" "${CMAKE_CURRENT_BINARY_DIR}/org.ladybird.Ladybird.metainfo.xml" @ONLY)
    install(FILES
        "${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        COMPONENT ladybird_Runtime
    )
    install(FILES
        "${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
        COMPONENT ladybird_Runtime
    )
    install(FILES
        "${FREEDESKTOP_RESOURCE_DIR}/org.ladybird.Ladybird.service"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/services"
        COMPONENT ladybird_Runtime
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/org.ladybird.Ladybird.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
        COMPONENT ladybird_Runtime
    )
endif()
