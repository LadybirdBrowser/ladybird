set(SOURCES
    BackgroundAction.cpp
    Thread.cpp
)

serenity_lib(LibThreading threading)
target_link_libraries(LibThreading PRIVATE LibCore)

if (WIN32)
    target_link_libraries(LibThreading PUBLIC ${PTHREAD_LIBRARY})
endif()
