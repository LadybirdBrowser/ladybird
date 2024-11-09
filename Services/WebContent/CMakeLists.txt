include(fontconfig)
include(pulseaudio)

set(SOURCES
    ${LADYBIRD_SOURCE_DIR}/Ladybird/FontPlugin.cpp
    ${LADYBIRD_SOURCE_DIR}/Ladybird/HelperProcess.cpp
    ${LADYBIRD_SOURCE_DIR}/Ladybird/ImageCodecPlugin.cpp
    ${LADYBIRD_SOURCE_DIR}/Ladybird/Utilities.cpp
    ConnectionFromClient.cpp
    ConsoleGlobalEnvironmentExtensions.cpp
    BackingStoreManager.cpp
    PageClient.cpp
    PageHost.cpp
    WebContentConsoleClient.cpp
    WebDriverConnection.cpp
)

if (ANDROID)
    add_library(webcontentservice SHARED
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Android/src/main/cpp/WebContentService.cpp
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Android/src/main/cpp/WebContentServiceJNI.cpp
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Android/src/main/cpp/JNIHelpers.cpp
        ${SOURCES}
    )
    target_link_libraries(webcontentservice PRIVATE android)
else()
    add_library(webcontentservice STATIC ${SOURCES})
    set_target_properties(webcontentservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()

target_include_directories(webcontentservice PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../..>)
target_include_directories(webcontentservice PUBLIC $<BUILD_INTERFACE:${LADYBIRD_SOURCE_DIR}/Services/>)
target_include_directories(webcontentservice PUBLIC $<BUILD_INTERFACE:${LADYBIRD_SOURCE_DIR}/Userland/>)

target_link_libraries(webcontentservice PUBLIC LibCore LibFileSystem LibGfx LibIPC LibJS LibMain LibMedia LibWeb LibWebSocket LibRequests LibWebView LibImageDecoderClient)

if (HAS_FONTCONFIG)
    target_link_libraries(webcontentservice PRIVATE Fontconfig::Fontconfig)
endif()

if (ENABLE_QT)
    qt_add_executable(WebContent
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Qt/EventLoopImplementationQt.cpp
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Qt/EventLoopImplementationQtEventTarget.cpp
        ${LADYBIRD_SOURCE_DIR}/Ladybird/Qt/StringUtils.cpp
        main.cpp
    )
    target_link_libraries(WebContent PRIVATE Qt::Core)
    target_compile_definitions(WebContent PRIVATE HAVE_QT=1)

    if (NOT HAVE_PULSEAUDIO)
        find_package(Qt6 REQUIRED COMPONENTS Multimedia)

        target_sources(WebContent PRIVATE
            ${LADYBIRD_SOURCE_DIR}/Ladybird/Qt/AudioCodecPluginQt.cpp
            ${LADYBIRD_SOURCE_DIR}/Ladybird/Qt/AudioThread.cpp
        )

        target_link_libraries(WebContent PRIVATE Qt::Multimedia)
        target_compile_definitions(WebContent PRIVATE HAVE_QT_MULTIMEDIA=1)
    endif()
else()
    add_executable(WebContent main.cpp)
endif()

target_link_libraries(WebContent PRIVATE webcontentservice LibURL)

target_sources(webcontentservice PUBLIC FILE_SET ladybird TYPE HEADERS
    BASE_DIRS ${LADYBIRD_SOURCE_DIR}
    FILES ${LADYBIRD_SOURCE_DIR}/Ladybird/FontPlugin.h
          ${LADYBIRD_SOURCE_DIR}/Ladybird/ImageCodecPlugin.h
)
target_sources(webcontentservice PUBLIC FILE_SET server TYPE HEADERS
    BASE_DIRS ${LADYBIRD_SOURCE_DIR}/Services
    FILES ConnectionFromClient.h
          ConsoleGlobalEnvironmentExtensions.h
          Forward.h
          PageHost.h
          WebContentConsoleClient.h
          WebDriverConnection.h
)
