include(accelerated_graphics)
set(WEBCONTENT_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/WebContent/)

set(WEBCONTENT_SOURCES
    ${WEBCONTENT_SOURCE_DIR}/ConnectionFromClient.cpp
    ${WEBCONTENT_SOURCE_DIR}/ConsoleGlobalEnvironmentExtensions.cpp
    ${WEBCONTENT_SOURCE_DIR}/PageClient.cpp
    ${WEBCONTENT_SOURCE_DIR}/PageHost.cpp
    ${WEBCONTENT_SOURCE_DIR}/WebContentConsoleClient.cpp
    ${WEBCONTENT_SOURCE_DIR}/WebDriverConnection.cpp
    ../FontPlugin.cpp
    ../HelperProcess.cpp
    ../ImageCodecPlugin.cpp
    ../Utilities.cpp
)

if (ENABLE_QT)
    qt_add_executable(WebContent ${WEBCONTENT_SOURCES})
    target_sources(WebContent PRIVATE
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
    set(LIB_TYPE STATIC)
    if (ANDROID OR IOS)
        set(LIB_TYPE SHARED)
    endif()
    add_library(webcontent ${LIB_TYPE} ${WEBCONTENT_SOURCES})
    target_include_directories(webcontent PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
    target_include_directories(webcontent PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
    target_include_directories(webcontent PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
    target_link_libraries(webcontent PRIVATE LibAudio LibCore LibFileSystem LibGfx LibIPC LibJS LibMain LibWeb LibWebSocket LibProtocol LibWebView LibImageDecoderClient)
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

    if (ANDROID)
        target_sources(webcontent PRIVATE
          ../Android/src/main/cpp/WebContentService.cpp
          ../Android/src/main/cpp/WebContentServiceJNI.cpp
          ../Android/src/main/cpp/LadybirdServiceBaseJNI.cpp
          ../Android/src/main/cpp/JNIHelpers.cpp
        )
        target_link_libraries(webcontent PRIVATE android)
    endif()

    add_executable(WebContent main.cpp)
    target_link_libraries(WebContent PRIVATE webcontent)
endif()

target_include_directories(WebContent PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
target_include_directories(WebContent PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/)
target_include_directories(WebContent PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_link_libraries(WebContent PRIVATE LibAudio LibCore LibFileSystem LibGfx LibImageDecoderClient LibIPC LibJS LibMain LibSQL LibWeb LibWebSocket LibProtocol LibWebView LibURL)

if (HAVE_PULSEAUDIO)
    target_compile_definitions(WebContent PRIVATE HAVE_PULSEAUDIO=1)
    if (TARGET webcontent)
        target_compile_definitions(webcontent PRIVATE HAVE_PULSEAUDIO=1)
    endif()
endif()

if (HAS_ACCELERATED_GRAPHICS)
    target_compile_definitions(WebContent PRIVATE HAS_ACCELERATED_GRAPHICS)
    target_link_libraries(WebContent PRIVATE LibAccelGfx)
    if (TARGET webcontent)
        target_compile_definitions(webcontent PRIVATE HAS_ACCELERATED_GRAPHICS)
        target_link_libraries(webcontent PRIVATE LibAccelGfx)
    endif()
endif()
