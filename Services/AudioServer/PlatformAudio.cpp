/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/PlatformAudio.h>

namespace Audio {

#if !defined(AUDIOSERVER_PLAYBACK_BACKEND)

NonnullRefPtr<PlaybackStream::CreatePromise> create_platform_playback_stream(OutputState, u32, PlaybackStream::AudioDataRequestCallback)
{
    return PlaybackStream::CreatePromise::rejected(Error::from_string_literal("No platform audio backend"));
}

#endif

#if !defined(AUDIOSERVER_CAPTURE_BACKEND)

NonnullRefPtr<RecordStream::CreatePromise> create_platform_record_stream(SampleSpecification const&, u32, StringView, RecordStream::RecordCallback)
{
    return RecordStream::CreatePromise::rejected(Error::from_string_literal("Audio capture is not supported on this platform"));
}

#endif

#if !defined(AUDIOSERVER_DEVICE_WATCH)

// FIXME: Implement device enumeration for the WASAPI (Windows) backend.
void watch_platform_audio_devices(AudioDeviceListCallback on_device_list)
{
    on_device_list(AudioDeviceEnumeration {});
}

#endif

}
