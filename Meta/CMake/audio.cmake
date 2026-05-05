include_guard()

# Audio backend -- how we output audio to the speakers.
if (APPLE AND NOT IOS)
    set(LADYBIRD_AUDIO_BACKEND "AUDIO_UNIT")
    return()
elseif (ANDROID)
    # PulseAudio is not available on Android; leave LADYBIRD_AUDIO_BACKEND unset
    # so that LibMedia falls back to the null PlaybackStream stub.
    return()
elseif (NOT WIN32)
    pkg_check_modules(PULSEAUDIO IMPORTED_TARGET libpulse)

    if (PULSEAUDIO_FOUND)
        set(LADYBIRD_AUDIO_BACKEND "PULSE")
        return()
    endif()
else()
    set(LADYBIRD_AUDIO_BACKEND "WASAPI")
    return()
endif()

message(WARNING "No audio backend available")
