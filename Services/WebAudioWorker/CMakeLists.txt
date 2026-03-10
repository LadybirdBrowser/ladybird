set(WEBAUDIOWORKER_SOURCES
    MainEventLoop.cpp
    SessionSampler.cpp
    SessionScriptProcessorHost.cpp
    WebAudioSessionConnection.cpp
    WebAudioRenderThread.cpp
    WebAudioSession.cpp
    WebAudioBrokerConnection.cpp
)

add_library(webaudioworkerservice STATIC ${WEBAUDIOWORKER_SOURCES})
set_target_properties(webaudioworkerservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(webaudioworkerservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(webaudioworkerservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(webaudioworkerservice PUBLIC LibAudioServer LibCore LibIPC LibMain LibThreading LibWeb LibWebAudio)

add_executable(WebAudioWorker main.cpp)

target_include_directories(WebAudioWorker PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(WebAudioWorker PRIVATE webaudioworkerservice)

if(WIN32)
    lagom_windows_bin(WebAudioWorker CONSOLE)
endif()
