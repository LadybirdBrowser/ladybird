if (NOT BUILD_TESTING)
    return()
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    return()
endif()

if (NOT LADYBIRD_GUI_FRAMEWORK STREQUAL "AppKit")
    # The accessibility surface these tests exercise is the AppKit port's NSAccessibility layer.
    return()
endif()

find_package(Python3 REQUIRED)

set(LADYBIRD_A11Y_BRIDGE_MAC_ENV
    "LADYBIRD_BINARY=$<TARGET_FILE:ladybird>"
)

add_test(
    NAME LibWebAccessibilityBridgeMacLayer1
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh
)
set_tests_properties(LibWebAccessibilityBridgeMacLayer1 PROPERTIES
    ENVIRONMENT "${LADYBIRD_A11Y_BRIDGE_MAC_ENV}"
    TIMEOUT 600
    TIMEOUT_SIGNAL_NAME SIGTERM
)
