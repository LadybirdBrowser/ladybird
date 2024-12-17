set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES
    ConnectionFromClient.cpp
)

if (ANDROID)
    add_library(imagedecoderservice SHARED
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/ImageDecoderService.cpp
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
        ${SOURCES}
    )
else()
    add_library(imagedecoderservice STATIC ${SOURCES})
endif()

add_executable(ImageDecoder main.cpp)

target_include_directories(imagedecoderservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(imagedecoderservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(ImageDecoder PRIVATE imagedecoderservice LibCore LibMain LibThreading)
target_link_libraries(imagedecoderservice PRIVATE LibCore LibGfx LibIPC LibImageDecoderClient LibMain LibThreading)
