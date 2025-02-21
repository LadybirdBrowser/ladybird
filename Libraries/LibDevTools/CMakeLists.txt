set(SOURCES
    Actor.cpp
    Actors/CSSPropertiesActor.cpp
    Actors/DeviceActor.cpp
    Actors/FrameActor.cpp
    Actors/HighlighterActor.cpp
    Actors/InspectorActor.cpp
    Actors/NodeActor.cpp
    Actors/PageStyleActor.cpp
    Actors/PreferenceActor.cpp
    Actors/ProcessActor.cpp
    Actors/RootActor.cpp
    Actors/TabActor.cpp
    Actors/TargetConfigurationActor.cpp
    Actors/ThreadActor.cpp
    Actors/ThreadConfigurationActor.cpp
    Actors/WalkerActor.cpp
    Actors/WatcherActor.cpp
    Connection.cpp
    DevToolsServer.cpp
)

serenity_lib(LibDevTools devtools)
target_link_libraries(LibDevTools PRIVATE LibCore LibWeb)
