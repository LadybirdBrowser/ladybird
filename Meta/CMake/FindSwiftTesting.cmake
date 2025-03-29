# Finds the swift-testing library
# On Apple platforms, this is a framework included in the Xcode release
include_guard()

if (NOT TARGET SwiftTesting::SwiftTesting)
  cmake_policy(PUSH)
  if (POLICY CMP0152)
    cmake_policy(SET CMP0152 NEW)
  endif()

  set(_SEARCH_PATHS "")
  set(_PLUGIN_PATHS "")
  foreach(path IN LISTS SWIFT_LIBRARY_SEARCH_PATHS)
    file(REAL_PATH ${path} real_path)
    if (EXISTS ${real_path})
      list(APPEND _SEARCH_PATHS ${real_path})
    endif()
    if (EXISTS "${real_path}/../host/plugins")
      file(REAL_PATH "${real_path}/../host/plugins" plugin_path)
      list(APPEND _PLUGIN_PATHS ${plugin_path})
      if (EXISTS "${plugin_path}/testing")
        list(APPEND _PLUGIN_PATHS "${plugin_path}/testing")
      endif()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _SEARCH_PATHS)
  list(REMOVE_DUPLICATES _PLUGIN_PATHS)

  find_library(SWIFT_TESTING NAMES Testing
    PATHS ${_SEARCH_PATHS}
  )
  if (SWIFT_TESTING)
    add_library(SwiftTesting::SwiftTesting IMPORTED UNKNOWN)
    message(STATUS "Found SwiftTesting: ${SWIFT_TESTING}")
    cmake_path(GET SWIFT_TESTING PARENT_PATH _SWIFT_TESTING_DIR)
    set_target_properties(SwiftTesting::SwiftTesting PROPERTIES
      IMPORTED_LOCATION "${SWIFT_TESTING}"
      INTERFACE_LINK_DIRECTORIES "${_SWIFT_TESTING_DIR}"
    )
    find_library(SWIFT_TESTING_MACROS NAMES TestingMacros
      PATHS ${_PLUGIN_PATHS}
      NO_DEFAULT_PATH
    )
    if (NOT SWIFT_TESTING_MACROS)
      message(FATAL_ERROR "Could not find associated TestingMacros plugin for ${SWIFT_TESTING}")
    endif()

    if (UNIX AND NOT APPLE)
      set_target_properties(SwiftTesting::SwiftTesting PROPERTIES
        INTERFACE_COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-load-plugin-library ${SWIFT_TESTING_MACROS}>"
        INTERFACE_LINK_OPTIONS "-load-plugin-library;${SWIFT_TESTING_MACROS}"
      )
    elseif (APPLE)
      cmake_path(GET SWIFT_TESTING_MACROS PARENT_PATH _SWIFT_TESTING_MACROS_DIR)
      set_target_properties(SwiftTesting::SwiftTesting PROPERTIES
        INTERFACE_LINK_OPTIONS "-F;${_SWIFT_TESTING_DIR};-plugin-path;${_SWIFT_TESTING_MACROS_DIR}"
      )
    endif()
    set(SwiftTesting_FOUND TRUE)
  endif()
  cmake_policy(POP)
endif()
