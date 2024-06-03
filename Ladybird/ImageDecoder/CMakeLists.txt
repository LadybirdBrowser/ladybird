set(IMAGE_DECODER_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/ImageDecoder)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(IMAGE_DECODER_SOURCES
    ${IMAGE_DECODER_SOURCE_DIR}/ConnectionFromClient.cpp
)

if (ANDROID)
    add_library(imagedecoder SHARED
            ${IMAGE_DECODER_SOURCES}
            ../Android/src/main/cpp/ImageDecoderService.cpp
            ../Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
            ../Utilities.cpp
            )
else()
    add_library(imagedecoder STATIC ${IMAGE_DECODER_SOURCES})
endif()

add_executable(ImageDecoder main.cpp)
target_link_libraries(ImageDecoder PRIVATE imagedecoder LibCore LibMain)

target_include_directories(imagedecoder PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
target_include_directories(imagedecoder PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_link_libraries(imagedecoder PRIVATE LibCore LibGfx LibIPC LibImageDecoderClient LibMain LibThreading)
