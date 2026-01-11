if (ANDROID OR VCPKG_TARGET_ANDROID)
    set(_possible_guis "Android")
    set(_default_gui "Android")
elseif (APPLE)
    set(_possible_guis "AppKit")
    set(_default_gui "AppKit")
elseif (LINUX)
    set(_possible_guis "GTK" "Qt")
    set(_default_gui "Qt")
else()
    set(_possible_guis "Qt")
    set(_default_gui "Qt")
endif()

# Note: ladybird_option() doesn't play nicely with this variable, so we use set() directly.
set(LADYBIRD_GUI_FRAMEWORK ${_default_gui} CACHE STRING "The GUI framework to use for the ladybird application. Possible values: ${_possible_guis}")

if (NOT "${LADYBIRD_GUI_FRAMEWORK}" IN_LIST _possible_guis)
    message(FATAL_ERROR "Invalid value for LADYBIRD_GUI_FRAMEWORK: ${LADYBIRD_GUI_FRAMEWORK}. Possible values: ${_possible_guis}")
endif()

if (LADYBIRD_GUI_FRAMEWORK STREQUAL "Qt")
    set(ENABLE_QT ON CACHE BOOL "Build ladybird application using Qt GUI" FORCE)
endif()
