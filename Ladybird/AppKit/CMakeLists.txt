add_library(ladybird_impl STATIC
    ${LADYBIRD_SOURCES}
    Application/Application.mm
    Application/ApplicationDelegate.mm
    Application/EventLoopImplementation.mm
    UI/Event.mm
    UI/Inspector.mm
    UI/InspectorController.mm
    UI/LadybirdWebView.mm
    UI/LadybirdWebViewBridge.cpp
    UI/LadybirdWebViewWindow.mm
    UI/Palette.mm
    UI/SearchPanel.mm
    UI/Tab.mm
    UI/TabController.mm
    Utilities/Conversions.mm
)
target_include_directories(ladybird_impl PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_compile_options(ladybird_impl PUBLIC
        $<$<COMPILE_LANGUAGE:CXX>:-fobjc-arc>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-anon-enum-enum-conversion> # Required for CGImageCreate
)
target_compile_features(ladybird_impl PUBLIC cxx_std_23)

if (ENABLE_SWIFT)
    target_sources(ladybird_impl PRIVATE
        UI/TaskManager.swift
        UI/TaskManagerController.swift
    )
    target_compile_definitions(ladybird_impl PUBLIC LADYBIRD_USE_SWIFT)
    set_target_properties(ladybird_impl PROPERTIES Swift_MODULE_NAME "SwiftLadybird")

    get_target_property(LADYBIRD_NATIVE_DIRS ladybird_impl INCLUDE_DIRECTORIES)
    _swift_generate_cxx_header(ladybird_impl "Ladybird-Swift.h"
        SEARCH_PATHS ${LADYBIRD_NATIVE_DIRS}
    )
else()
    target_sources(ladybird_impl PRIVATE
        UI/TaskManager.mm
        UI/TaskManagerController.mm
    )
endif()

add_executable(ladybird MACOSX_BUNDLE
   main.mm
)
target_link_libraries(ladybird_impl PUBLIC "-framework Cocoa -framework UniformTypeIdentifiers" LibUnicode)
target_link_libraries(ladybird PRIVATE ladybird_impl)

create_ladybird_bundle(ladybird)
