include(audio)

set(AUDIOSERVER_SOURCES
    Server.cpp
    BrokerConnection.cpp
    InputStream.cpp
    OutputStream.cpp
    SessionConnection.cpp
)

if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    list(APPEND AUDIOSERVER_SOURCES
        Platform/PulseAudio.cpp
        Platform/PulseAudioInputStream.cpp
        Platform/PulseAudioOutputDriver.cpp
    )
    set(AUDIOSERVER_COMPILE_DEFINITIONS HAVE_PULSEAUDIO=1)
    set(AUDIOSERVER_EXTRA_LIBRARIES PkgConfig::PULSEAUDIO)
elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "AUDIO_UNIT")
    list(APPEND AUDIOSERVER_SOURCES
        Platform/CoreAudio.cpp
        Platform/CoreAudioInputStream.cpp
        Platform/CoreAudioOutputDriver.cpp
    )
    find_library(COREAUDIO CoreAudio REQUIRED)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(AUDIOTOOLBOX AudioToolbox REQUIRED)
    set(AUDIOSERVER_EXTRA_LIBRARIES ${COREAUDIO} ${COREFOUNDATION} ${AUDIOTOOLBOX})
elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "WASAPI")
    list(APPEND AUDIOSERVER_SOURCES
        Platform/Wasapi.cpp
        Platform/WasapiInputStream.cpp
        Platform/WasapiOutputDriver.cpp
    )
    set(AUDIOSERVER_EXTRA_LIBRARIES ole32 avrt winmm)
elseif (DEFINED LADYBIRD_AUDIO_BACKEND)
    message(FATAL_ERROR "Please update ${CMAKE_CURRENT_LIST_FILE} for audio driver ${LADYBIRD_AUDIO_BACKEND}")
else()
    list(APPEND AUDIOSERVER_SOURCES
        Platform/StubPlatform.cpp
    )
endif()

add_library(audioserverservice STATIC ${AUDIOSERVER_SOURCES})
set_target_properties(audioserverservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

if (AUDIOSERVER_COMPILE_DEFINITIONS)
    target_compile_definitions(audioserverservice PRIVATE ${AUDIOSERVER_COMPILE_DEFINITIONS})
endif()

if (AUDIOSERVER_EXTRA_LIBRARIES)
    target_link_libraries(audioserverservice PRIVATE ${AUDIOSERVER_EXTRA_LIBRARIES})
endif()

target_include_directories(audioserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(audioserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(audioserverservice PUBLIC LibAudioServer LibCore LibIPC LibMain LibThreading)

add_executable(AudioServer main.cpp)

target_include_directories(AudioServer PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(AudioServer PRIVATE audioserverservice)

if(WIN32)
    lagom_windows_bin(AudioServer CONSOLE)
endif()
