set(SOURCES
    Actor.cpp
    Actors/AccessibilityActor.cpp
    Actors/AccessibilityNodeActor.cpp
    Actors/AccessibilityWalkerActor.cpp
    Actors/BlackboxingActor.cpp
    Actors/BreakpointListActor.cpp
    Actors/ConsoleActor.cpp
    Actors/CookiesActor.cpp
    Actors/CSSPropertiesActor.cpp
    Actors/DebuggerFrameActor.cpp
    Actors/DeviceActor.cpp
    Actors/EnvironmentActor.cpp
    Actors/FrameActor.cpp
    Actors/HighlighterActor.cpp
    Actors/IndexedDBActor.cpp
    Actors/InspectorActor.cpp
    Actors/LayoutInspectorActor.cpp
    Actors/NetworkEventActor.cpp
    Actors/NetworkParentActor.cpp
    Actors/NodeActor.cpp
    Actors/ObjectActor.cpp
    Actors/PageStyleActor.cpp
    Actors/ParentAccessibilityActor.cpp
    Actors/PreferenceActor.cpp
    Actors/ProcessActor.cpp
    Actors/PropertyIteratorActor.cpp
    Actors/RootActor.cpp
    Actors/SourceActor.cpp
    Actors/StyleRuleActor.cpp
    Actors/StyleSheetsActor.cpp
    Actors/SymbolIteratorActor.cpp
    Actors/StorageActor.cpp
    Actors/TabActor.cpp
    Actors/TargetConfigurationActor.cpp
    Actors/ThreadActor.cpp
    Actors/ThreadConfigurationActor.cpp
    Actors/WalkerActor.cpp
    Actors/WatcherActor.cpp
    Connection.cpp
    DevToolsServer.cpp
    FirefoxClient.cpp
    IndexedDBSerialization.cpp
    StorageHelpers.cpp
)

if (APPLE)
    list(APPEND SOURCES LaunchApplicationMacOS.mm)
    set_source_files_properties(LaunchApplicationMacOS.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

ladybird_lib(LibDevTools devtools EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibDevTools PRIVATE LibCore LibFileSystem LibHTTP LibWeb LibURL)

if (APPLE)
    target_link_libraries(LibDevTools PRIVATE "-framework AppKit")
endif()
