include(audio)

set(SOURCES
    CompositorConnection.cpp
    ConnectionFromClient.cpp
    ConsoleGlobalEnvironmentExtensions.cpp
    DevToolsConsoleClient.cpp
    PageClient.cpp
    PageHost.cpp
    WebContentCompositorHost.cpp
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

target_link_libraries(webcontentservice PUBLIC LibCore LibCrypto LibDevTools LibFileSystem LibGfx LibHTTP LibIPC LibJS LibMain LibMedia LibWasm LibWeb LibWebSocket LibRequests LibWebView LibImageDecoderClient LibGC)
target_link_libraries(webcontentservice PRIVATE OpenSSL::Crypto OpenSSL::SSL)
target_link_libraries(webcontentservice PRIVATE SDL3::SDL3)
target_compile_options(webcontentservice PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wexit-time-destructors>)

add_executable(WebContent main.cpp)

if (LINUX)
    target_sources(WebContent PRIVATE ../RendererSandboxLinux.cpp)
elseif (APPLE)
    target_sources(WebContent PRIVATE ../RendererSandboxMacOS.cpp)
else()
    target_sources(WebContent PRIVATE ../RendererSandboxUnimplemented.cpp)
endif()

target_link_libraries(WebContent PRIVATE webcontentservice LibSandbox LibURL)
target_compile_options(WebContent PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wexit-time-destructors>)

if(WIN32)
    ladybird_windows_bin(WebContent CONSOLE)
endif()


target_sources(webcontentservice PUBLIC FILE_SET server TYPE HEADERS
    BASE_DIRS ${LADYBIRD_SOURCE_DIR}/Services
    FILES ConnectionFromClient.h
          ConsoleGlobalEnvironmentExtensions.h
          Forward.h
          PageHost.h
          WebContentCompositorHost.h
          WebContentConsoleClient.h
          WebDriverConnection.h
)
