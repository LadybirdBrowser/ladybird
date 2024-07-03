include(fontconfig)

set(WEBCONTENT_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/WebContent/)

set(WEBCONTENT_SOURCES
    ${WEBCONTENT_SOURCE_DIR}/ConnectionFromClient.cpp
    ${WEBCONTENT_SOURCE_DIR}/ConsoleGlobalEnvironmentExtensions.cpp
    ${WEBCONTENT_SOURCE_DIR}/BackingStoreManager.cpp
    ${WEBCONTENT_SOURCE_DIR}/PageClient.cpp
    ${WEBCONTENT_SOURCE_DIR}/PageHost.cpp
    ${WEBCONTENT_SOURCE_DIR}/WebContentConsoleClient.cpp
    ${WEBCONTENT_SOURCE_DIR}/WebDriverConnection.cpp
    ../FontPlugin.cpp
    ../HelperProcess.cpp
    ../ImageCodecPlugin.cpp
    ../Utilities.cpp
)

if (ANDROID)
    add_library(webcontent SHARED
        ${WEBCONTENT_SOURCES}
        ../Android/src/main/cpp/WebContentService.cpp
        ../Android/src/main/cpp/WebContentServiceJNI.cpp
        ../Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
        ../Android/src/main/cpp/JNIHelpers.cpp
    )
    target_link_libraries(webcontent PRIVATE android)
else()
    add_library(webcontent STATIC ${WEBCONTENT_SOURCES})
    set_target_properties(webcontent PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()

target_include_directories(webcontent PUBLIC $<BUILD_INTERFACE:${LADYBIRD_SOURCE_DIR}/Userland/Services/>)
target_include_directories(webcontent PUBLIC $<BUILD_INTERFACE:${LADYBIRD_SOURCE_DIR}/Userland/>)
target_include_directories(webcontent PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/..>)

target_link_libraries(webcontent PUBLIC LibAudio LibCore LibFileSystem LibGfx LibIPC LibJS LibMain LibWeb LibWebSocket LibProtocol LibWebView LibImageDecoderClient)

if (HAVE_PULSEAUDIO)
    target_compile_definitions(webcontent PUBLIC HAVE_PULSEAUDIO=1)
endif()

if (HAS_FONTCONFIG)
    target_link_libraries(webcontent PRIVATE Fontconfig::Fontconfig)
endif()

if (ENABLE_QT)
    qt_add_executable(WebContent
        ../Qt/EventLoopImplementationQt.cpp
        ../Qt/EventLoopImplementationQtEventTarget.cpp
        ../Qt/RequestManagerQt.cpp
        ../Qt/StringUtils.cpp
        ../Qt/WebSocketQt.cpp
        ../Qt/WebSocketImplQt.cpp
        main.cpp
    )
    target_link_libraries(WebContent PRIVATE Qt::Core Qt::Network)
    target_compile_definitions(WebContent PRIVATE HAVE_QT=1)

    if (NOT HAVE_PULSEAUDIO)
        find_package(Qt6 REQUIRED COMPONENTS Multimedia)

        target_sources(WebContent PRIVATE
            ../Qt/AudioCodecPluginQt.cpp
            ../Qt/AudioThread.cpp
        )

        target_link_libraries(WebContent PRIVATE Qt::Multimedia)
        target_compile_definitions(WebContent PRIVATE HAVE_QT_MULTIMEDIA=1)
    endif()
else()
    add_executable(WebContent main.cpp)
endif()

target_link_libraries(WebContent PRIVATE webcontent LibURL)

target_sources(webcontent PUBLIC FILE_SET ladybird TYPE HEADERS
    BASE_DIRS ${LADYBIRD_SOURCE_DIR}
    FILES ../FontPlugin.h
          ../ImageCodecPlugin.h
)
target_sources(webcontent PUBLIC FILE_SET server TYPE HEADERS
    BASE_DIRS ${LADYBIRD_SOURCE_DIR}/Userland/Services
    FILES ${WEBCONTENT_SOURCE_DIR}/ConnectionFromClient.h
          ${WEBCONTENT_SOURCE_DIR}/ConsoleGlobalEnvironmentExtensions.h
          ${WEBCONTENT_SOURCE_DIR}/Forward.h
          ${WEBCONTENT_SOURCE_DIR}/PageHost.h
          ${WEBCONTENT_SOURCE_DIR}/WebContentConsoleClient.h
          ${WEBCONTENT_SOURCE_DIR}/WebDriverConnection.h
)
