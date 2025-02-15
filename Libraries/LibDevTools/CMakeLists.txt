set(SOURCES
    Actor.cpp
    Actors/DeviceActor.cpp
    Actors/PreferenceActor.cpp
    Actors/ProcessActor.cpp
    Actors/RootActor.cpp
    Connection.cpp
    DevToolsServer.cpp
)

serenity_lib(LibDevTools devtools)
target_link_libraries(LibDevTools PRIVATE LibCore LibWeb)
