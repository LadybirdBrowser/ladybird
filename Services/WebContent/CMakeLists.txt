include(audio)

set(SOURCES
    BackingStoreManager.cpp
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

if (ENABLE_QT)
    qt_add_executable(WebContent main.cpp)
    target_link_libraries(WebContent PRIVATE Qt::Core)
    target_compile_definitions(WebContent PRIVATE HAVE_QT=1)

    if (NOT DEFINED LADYBIRD_AUDIO_BACKEND)
        find_package(Qt6 REQUIRED COMPONENTS Multimedia)

        target_sources(WebContent PRIVATE
            ${LADYBIRD_SOURCE_DIR}/UI/Qt/AudioCodecPluginQt.cpp
            ${LADYBIRD_SOURCE_DIR}/UI/Qt/AudioThread.cpp
        )

        target_link_libraries(WebContent PRIVATE Qt::Multimedia)
        target_compile_definitions(WebContent PRIVATE HAVE_QT_MULTIMEDIA=1)
    endif()
else()
    add_executable(WebContent main.cpp)
endif()

target_link_libraries(WebContent PRIVATE webcontentservice LibURL)

target_sources(webcontentservice PUBLIC FILE_SET server TYPE HEADERS
    BASE_DIRS ${LADYBIRD_SOURCE_DIR}/Services
    FILES ConnectionFromClient.h
          ConsoleGlobalEnvironmentExtensions.h
          Forward.h
          PageHost.h
          WebContentConsoleClient.h
          WebDriverConnection.h
)
