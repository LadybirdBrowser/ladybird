set(SOURCES
    BackgroundAction.cpp
    Thread.cpp
)

serenity_lib(LibThreading threading)
target_link_libraries(LibThreading PRIVATE LibCore)
