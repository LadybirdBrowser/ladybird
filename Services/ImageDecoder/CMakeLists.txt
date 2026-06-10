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
    target_link_libraries(imagedecoderservice PRIVATE LibWebView)
else()
    add_library(imagedecoderservice STATIC ${SOURCES})
endif()

add_executable(ImageDecoder main.cpp)

if (LINUX)
    target_sources(ImageDecoder PRIVATE SandboxLinux.cpp)
    if (ENABLE_ADDRESS_SANITIZER)
        target_sources(ImageDecoder PRIVATE LeakSanitizer.cpp)
    endif()
elseif (APPLE)
    target_sources(ImageDecoder PRIVATE SandboxMacOS.cpp)
else()
    target_sources(ImageDecoder PRIVATE SandboxUnimplemented.cpp)
endif()

target_include_directories(imagedecoderservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(imagedecoderservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(ImageDecoder PRIVATE imagedecoderservice LibCore LibMain LibSandbox LibThreading)
target_link_libraries(imagedecoderservice PRIVATE LibCore LibGfx LibImageDecoders LibIPC LibImageDecoderClient LibMain LibSync LibThreading)

if (WIN32)
    ladybird_windows_bin(ImageDecoder CONSOLE)
endif()
