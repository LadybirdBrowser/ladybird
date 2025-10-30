set(SOURCES
    BackgroundAction.cpp
    Thread.cpp
    ThreadPool.cpp
)

ladybird_lib(LibThreading threading)
target_link_libraries(LibThreading PRIVATE LibCore LibSync)

if (WIN32)
    target_include_directories(LibThreading PUBLIC $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    target_link_libraries(LibThreading PUBLIC ${PTHREAD_LIBRARY})
endif()
