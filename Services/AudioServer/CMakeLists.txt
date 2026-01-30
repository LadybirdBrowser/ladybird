set(AUDIOSERVER_SOURCES
    AudioInputDeviceManager.cpp
    AudioInputStreamManager.cpp
    AudioOutputDeviceManager.cpp
    AudioOutputDevice.cpp
    ConnectionLifecycle.cpp
    AudioServerConnection.cpp
)

add_library(audioserverservice STATIC ${AUDIOSERVER_SOURCES})
set_target_properties(audioserverservice PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

target_include_directories(audioserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(audioserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(audioserverservice PUBLIC LibCore LibIPC LibMain LibMedia LibThreading)

if (APPLE)
    find_library(COREAUDIO CoreAudio REQUIRED)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(AUDIOTOOLBOX AudioToolbox REQUIRED)
    target_link_libraries(audioserverservice PRIVATE ${COREAUDIO} ${COREFOUNDATION} ${AUDIOTOOLBOX})
endif()

if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    target_compile_definitions(audioserverservice PRIVATE HAVE_PULSEAUDIO=1)
    target_link_libraries(audioserverservice PRIVATE PkgConfig::PULSEAUDIO)
endif()

add_executable(AudioServer main.cpp)

# FIXME: Add Android service

target_include_directories(AudioServer PRIVATE ${LADYBIRD_SOURCE_DIR})
target_link_libraries(AudioServer PRIVATE audioserverservice)

if(WIN32)
    lagom_windows_bin(AudioServer CONSOLE)
endif()
