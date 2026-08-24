add_library(ladybird_impl STATIC
    Application/Application.mm
    Application/ApplicationDelegate.mm
    Application/ClipboardTypes.mm
    Application/EventLoopImplementationMacOS.mm
    Interface/Autocomplete.mm
    Interface/BookmarkFolder.mm
    Interface/BookmarksBar.mm
    Interface/Event.mm
    Interface/InfoBar.mm
    Interface/LadybirdWebView.mm
    Interface/LadybirdWebViewBridge.cpp
    Interface/LadybirdWebViewWindow.mm
    Interface/LocationSearchField.mm
    Interface/Menu.mm
    Interface/Palette.mm
    Interface/SearchPanel.mm
    Interface/Tab.mm
    Interface/TabController.mm
    Utilities/ApplicationIcon.mm
    Utilities/Conversions.mm
    Utilities/DictionaryLookup.mm
    Utilities/ExternalURLHandler.mm
)
target_include_directories(ladybird_impl PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_compile_options(ladybird_impl PUBLIC
        $<$<COMPILE_LANGUAGE:CXX>:-fobjc-arc>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-anon-enum-enum-conversion> # Required for CGImageCreate
)
target_compile_features(ladybird_impl PUBLIC cxx_std_23)

add_executable(ladybird MACOSX_BUNDLE
   main.mm
)
target_link_libraries(ladybird_impl PUBLIC "-framework Cocoa -framework Metal -framework QuartzCore -framework UniformTypeIdentifiers" LibUnicode)
target_link_libraries(ladybird PRIVATE ladybird_impl)

# FIXME: We clear this directory to work around an upstream vcpkg bug and ensure it repopulates from the dependencies
# actually being linked against — avoiding a dyld rejection. See #11299. The right place to fix this is vcpkg upstream.
add_custom_command(TARGET ladybird PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "$<TARGET_BUNDLE_DIR:ladybird>/Contents/Frameworks"
    VERBATIM
)

create_ladybird_bundle(ladybird)
