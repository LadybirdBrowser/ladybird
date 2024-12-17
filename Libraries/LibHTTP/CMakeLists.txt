set(SOURCES
    HttpRequest.cpp
    HttpResponse.cpp
)

serenity_lib(LibHTTP http)
target_link_libraries(LibHTTP PRIVATE LibCompress LibCore LibTLS LibURL)

if (WIN32)
    find_package(pthread REQUIRED)
    target_include_directories(LibHTTP PRIVATE ${PTHREAD_INCLUDE_DIR})
endif()
