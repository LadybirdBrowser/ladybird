include(audio)
include(SDL3)

set(SOURCES
    ConnectionFromClient.cpp
    ConsoleGlobalEnvironmentExtensions.cpp
    DevToolsConsoleClient.cpp
    PageClient.cpp
    PageHost.cpp
    WebContentConsoleClient.cpp
    WebDriverConnection.cpp
    WebUIConnection.cpp
)

if (ANDROID)
    add_library(webcontentservice SHARED
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/WebContentService.cpp
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/WebContentServiceJNI.cpp
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
        ${LADYBIRD_SOURCE_DIR}/UI/Android/src/main/cpp/JNIHelpers.cpp
        ${SOURCES}
    )
    target_link_libraries(webcontentservice PRIVATE android)
else()
    add_library(webcontentservice STATIC ${SOURCES})
    set_target_properties(webcontentservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()

target_include_directories(webcontentservice PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../..>)
target_include_directories(webcontentservice PUBLIC $<BUILD_INTERFACE:${LADYBIRD_SOURCE_DIR}>)
target_include_directories(webcontentservice PUBLIC $<BUILD_INTERFACE:${LADYBIRD_SOURCE_DIR}/Services/>)

target_link_libraries(webcontentservice PUBLIC LibCore LibCrypto LibFileSystem LibGfx LibIPC LibJS LibMain LibMedia LibWeb LibWebSocket LibRequests LibWebView LibImageDecoderClient LibGC)
target_link_libraries(webcontentservice PRIVATE OpenSSL::Crypto OpenSSL::SSL)
target_link_libraries(webcontentservice PRIVATE SDL3::SDL3)

add_executable(WebContent main.cpp)

target_link_libraries(WebContent PRIVATE webcontentservice LibURL)

if(WIN32)
    find_package(unofficial-angle REQUIRED CONFIG)
    find_package(SQLite3 REQUIRED)
    target_link_libraries(WebContent PRIVATE LibMedia LibTextCodec SQLite::SQLite3 unofficial::angle::libGLESv2)
    lagom_windows_bin(WebContent)
endif()


target_sources(webcontentservice PUBLIC FILE_SET server TYPE HEADERS
    BASE_DIRS ${LADYBIRD_SOURCE_DIR}/Services
    FILES ConnectionFromClient.h
          ConsoleGlobalEnvironmentExtensions.h
          Forward.h
          PageHost.h
          WebContentConsoleClient.h
          WebDriverConnection.h
)
