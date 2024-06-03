
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(package Ladybird)

set(ladybird_applications ladybird ${ladybird_helper_processes})

set(app_install_targets ${ladybird_applications})

install(TARGETS ladybird
  EXPORT ladybirdTargets
  RUNTIME
    COMPONENT ladybird_Runtime
    DESTINATION ${CMAKE_INSTALL_BINDIR}
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
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)

include("${LADYBIRD_SOURCE_DIR}/Meta/Lagom/get_linked_lagom_libraries.cmake")
foreach (application IN LISTS ladybird_applications)
  get_linked_lagom_libraries("${application}" "${application}_lagom_libraries")
  list(APPEND all_required_lagom_libraries "${${application}_lagom_libraries}")
endforeach()
list(REMOVE_DUPLICATES all_required_lagom_libraries)

# Remove ladybird shlib if it exists
list(REMOVE_ITEM all_required_lagom_libraries ladybird)

# Install service impl libraries if they exist
macro(install_service_lib service)
    if (TARGET ${service})
      get_target_property(target_type ${service} TYPE)
      if ("${target_type}" STREQUAL STATIC_LIBRARY)
          list(APPEND all_required_lagom_libraries ${service})
      endif()
    endif()
endmacro()
foreach(service IN LISTS webcontent requestserver websocket webworker)
    install_service_lib(${service})
endforeach()

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
    FILES "${PROJECT_BINARY_DIR}/${package}ConfigVersion.cmake"
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

if (APPLE)
  # Fixup the app bundle and copy:
  #   - Libraries from lib/ to Ladybird.app/Contents/lib
  install(CODE "
    set(lib_dir \${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR})
    if (IS_ABSOLUTE ${CMAKE_INSTALL_LIBDIR})
      set(lib_dir ${CMAKE_INSTALL_LIBDIR})
    endif()

    set(contents_dir \${CMAKE_INSTALL_PREFIX}/bundle/Ladybird.app/Contents)
    file(COPY \${lib_dir} DESTINATION \${contents_dir})
  "
  COMPONENT ladybird_Runtime)
endif()
