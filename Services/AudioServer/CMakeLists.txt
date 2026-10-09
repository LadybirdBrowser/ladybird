include(audio)

set(SOURCES
    CaptureDevices.cpp
    ControlConnection.cpp
    PlatformAudio.cpp
    PlaybackStreamMixer.cpp
    ServerConnection.cpp
    Tabs.cpp
)

add_library(audioserverservice STATIC ${SOURCES})
add_executable(AudioServer main.cpp)

if (LINUX)
    target_sources(AudioServer PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(AudioServer PRIVATE SandboxMacOS.cpp)
else()
    target_sources(AudioServer PRIVATE SandboxUnimplemented.cpp)
endif()

add_dependencies(audioserverservice
    generate_AudioServerControlClientEndpoint.h
    generate_AudioServerControlEndpoint.h
)

target_include_directories(audioserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(audioserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(AudioServer PRIVATE audioserverservice LibCore LibMain LibMedia LibSandbox)
target_link_libraries(audioserverservice PRIVATE LibCore LibIPC LibMedia LibThreading)
target_link_libraries(AudioServer PRIVATE LibThreading)

# The platform backends: the only code in the tree that opens an audio device.
if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    target_sources(audioserverservice PRIVATE
        AudioDevicesPulseAudio.cpp
        PlaybackStreamPulseAudio.cpp
        PulseAudioWrappers.cpp
        RecordStreamPulseAudio.cpp
    )
    target_compile_definitions(audioserverservice PRIVATE AUDIOSERVER_PLAYBACK_BACKEND=1 AUDIOSERVER_CAPTURE_BACKEND=1 AUDIOSERVER_DEVICE_WATCH=1)
    target_link_libraries(audioserverservice PRIVATE PkgConfig::PULSEAUDIO)
elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "AUDIO_UNIT")
    target_sources(audioserverservice PRIVATE
        AudioDevicesCoreAudio.cpp
        PlaybackStreamAudioUnit.cpp
        RecordStreamAudioUnit.cpp
    )
    target_compile_definitions(audioserverservice PRIVATE AUDIOSERVER_PLAYBACK_BACKEND=1 AUDIOSERVER_CAPTURE_BACKEND=1 AUDIOSERVER_DEVICE_WATCH=1)
    find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
    find_library(CORE_AUDIO CoreAudio REQUIRED)
    find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
    target_link_libraries(audioserverservice PRIVATE ${AUDIO_TOOLBOX} ${CORE_AUDIO} ${CORE_FOUNDATION})
elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "WASAPI")
    target_sources(audioserverservice PRIVATE PlaybackStreamWasapi.cpp)
    target_compile_definitions(audioserverservice PRIVATE AUDIOSERVER_PLAYBACK_BACKEND=1)
    target_link_libraries(audioserverservice PRIVATE ole32 avrt winmm)
elseif (DEFINED LADYBIRD_AUDIO_BACKEND)
    message(FATAL_ERROR "Please update ${CMAKE_CURRENT_LIST_FILE} for audio backend ${LADYBIRD_AUDIO_BACKEND}")
endif()

if (WIN32)
    ladybird_windows_bin(AudioServer CONSOLE)
endif()

ladybird_sign_helper_process(AudioServer)
