if (NOT BUILD_TESTING)
    return()
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

if (NOT LADYBIRD_GUI_FRAMEWORK STREQUAL "Qt")
    # The accessibility bridge these tests exercise is part of the Qt port.
    return()
endif()

if (ENABLE_ADDRESS_SANITIZER)
    # For running these tests under CI:
    # 1. We need to actually launch a Qt-UI Ladybird process, and assert on the output from the Qt AT-SPI2 bridge.
    # 2. Qt's AT-SPI2 bridge only activates with a real platform plugin (offscreen doesn’t qualify).
    # 3. On a headless Linux runner in our CI, the only viable Qt platform plugin is xcb.
    # 4. Qt's xcb plugin pulls libxkbcommon-x11 in as a transitive runtime dependency.
    # But, under ASan, the QXcbConnection startup path triggers a buffer overflow in libxkbcommon-x11. Therefore, we
    # skip these tests under ASan — and for CI, we run them on the Release runner instead.
    return()
endif()

# run_tests.sh reads LADYBIRD_BINARY to locate the Ladybird executable. Point it at the ladybird target — so tests don't
# depend on the Build/release layout.
#
# Note that CI=1 is intentionally not set here. This isn't the right place to set it. And anyway, run_tests.sh already
# enables its isolated Xvfb + scratch-D-Bus + AT-SPI2 path when CI is set in the environment (which GitHub Actions does
# automatically via CI=true).
set(LADYBIRD_A11Y_BRIDGE_ENV
    "LADYBIRD_BINARY=$<TARGET_FILE:ladybird>"
)

add_test(
    NAME LibWebAccessibilityBridgeLayer1
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh --layer=1
)
set_tests_properties(LibWebAccessibilityBridgeLayer1 PROPERTIES
    ENVIRONMENT "${LADYBIRD_A11Y_BRIDGE_ENV}"
    TIMEOUT 600
    TIMEOUT_SIGNAL_NAME SIGTERM
)

add_test(
    NAME LibWebAccessibilityBridgeLayer2
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh --layer=2
)
set_tests_properties(LibWebAccessibilityBridgeLayer2 PROPERTIES
    ENVIRONMENT "${LADYBIRD_A11Y_BRIDGE_ENV}"
    TIMEOUT 600
    TIMEOUT_SIGNAL_NAME SIGTERM
)
