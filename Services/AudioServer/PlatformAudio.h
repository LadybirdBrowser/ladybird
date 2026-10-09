/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/StringView.h>
#include <LibMedia/Audio/AudioDevices.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Audio/RecordStream.h>
#include <LibMedia/Audio/SampleSpecification.h>

// The platform's own devices, which only the AudioServer opens. Each is defined by the audio backend that is built in,
// and a build without one has a stand-in that rejects or reports no devices.
namespace Audio {

NonnullRefPtr<PlaybackStream::CreatePromise> create_platform_playback_stream(OutputState, u32 target_latency_ms, PlaybackStream::AudioDataRequestCallback);

// The backend honors the requested specification by converting where it can.
NonnullRefPtr<RecordStream::CreatePromise> create_platform_record_stream(SampleSpecification const&, u32 fragment_size_bytes, StringView device_id, RecordStream::RecordCallback);

// Reports the devices once a list is available and again on every change, on the event loop of the calling thread.
void watch_platform_audio_devices(AudioDeviceListCallback);

}
