set(SOURCES
    BackgroundAction.cpp
    Thread.cpp
)

serenity_lib(LibThreading threading)
target_link_libraries(LibThreading PRIVATE LibCore)

if (WIN32)
    find_package(pthread REQUIRED)
    target_include_directories(LibThreading PUBLIC $<BUILD_INTERFACE:${PTHREAD_INCLUDE_DIR}>)
    target_link_libraries(LibThreading PUBLIC ${PTHREAD_LIBRARY})
endif()
