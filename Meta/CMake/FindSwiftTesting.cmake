# Finds the swift-testing library
# On Apple platforms, this is a framework included in the Xcode release

find_library(SWIFT_TESTING NAMES Testing
  PATHS ${SWIFT_LIBRARY_SEARCH_PATHS}
)
if (SWIFT_TESTING)
    if (NOT TARGET SwiftTesting::SwiftTesting)
      add_library(SwiftTesting::SwiftTesting IMPORTED UNKNOWN)
      message(STATUS "Found SwiftTesting: ${SWIFT_TESTING}")
      cmake_path(GET SWIFT_TESTING PARENT_PATH _SWIFT_TESTING_DIR)
      set_target_properties(SwiftTesting::SwiftTesting PROPERTIES
        IMPORTED_LOCATION "${SWIFT_TESTING}"
        INTERFACE_LINK_DIRECTORIES "${_SWIFT_TESTING_DIR}"
      )
      if (UNIX AND NOT APPLE)
        cmake_path(GET _SWIFT_TESTING_DIR PARENT_PATH _SWIFT_TESTING_TARGETLESS_DIR)
        set_target_properties(SwiftTesting::SwiftTesting PROPERTIES
          INTERFACE_COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-load-plugin-library ${_SWIFT_TESTING_TARGETLESS_DIR}/host/plugins/libTestingMacros.so>"
          INTERFACE_LINK_OPTIONS "-load-plugin-library;${_SWIFT_TESTING_TARGETLESS_DIR}/host/plugins/libTestingMacros.so"
        )
      endif()
    endif()
    set(SwiftTesting_FOUND TRUE)
endif()
