add_library(ladybird_impl STATIC
    ${LADYBIRD_SOURCES}
    Application/Application.mm
    Application/ApplicationDelegate.mm
    Interface/Autocomplete.mm
    Interface/Event.mm
    Interface/InfoBar.mm
    Interface/LadybirdWebView.mm
    Interface/LadybirdWebViewBridge.cpp
    Interface/LadybirdWebViewWindow.mm
    Interface/Palette.mm
    Interface/SearchPanel.mm
    Interface/Tab.mm
    Interface/TabController.mm
    Utilities/Conversions.mm
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
target_link_libraries(ladybird_impl PUBLIC "-framework Cocoa -framework UniformTypeIdentifiers" LibUnicode)
target_link_libraries(ladybird PRIVATE ladybird_impl)

create_ladybird_bundle(ladybird)
