set(SOURCES
    Actor.cpp
    Actors/AccessibilityActor.cpp
    Actors/AccessibilityNodeActor.cpp
    Actors/AccessibilityWalkerActor.cpp
    Actors/ConsoleActor.cpp
    Actors/CSSPropertiesActor.cpp
    Actors/DeviceActor.cpp
    Actors/FrameActor.cpp
    Actors/HighlighterActor.cpp
    Actors/InspectorActor.cpp
    Actors/LayoutInspectorActor.cpp
    Actors/NetworkEventActor.cpp
    Actors/NetworkParentActor.cpp
    Actors/NodeActor.cpp
    Actors/PageStyleActor.cpp
    Actors/ParentAccessibilityActor.cpp
    Actors/PreferenceActor.cpp
    Actors/ProcessActor.cpp
    Actors/RootActor.cpp
    Actors/StyleSheetsActor.cpp
    Actors/TabActor.cpp
    Actors/TargetConfigurationActor.cpp
    Actors/ThreadActor.cpp
    Actors/ThreadConfigurationActor.cpp
    Actors/WalkerActor.cpp
    Actors/WatcherActor.cpp
    Connection.cpp
    DevToolsServer.cpp
)

ladybird_lib(LibDevTools devtools EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibDevTools PRIVATE LibCore LibWeb)
