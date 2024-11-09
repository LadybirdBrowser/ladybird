set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES
    ConnectionFromClient.cpp
)

if (ANDROID)
    add_library(imagedecoderservice SHARED
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Android/src/main/cpp/ImageDecoderService.cpp
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Utilities.cpp
        ${SOURCES}
    )
else()
    add_library(imagedecoderservice STATIC ${SOURCES})
endif()

add_executable(ImageDecoder main.cpp)

target_include_directories(imagedecoderservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(imagedecoderservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(ImageDecoder PRIVATE imagedecoderservice LibCore LibMain)
target_link_libraries(imagedecoderservice PRIVATE LibCore LibGfx LibIPC LibImageDecoderClient LibMain LibThreading)
