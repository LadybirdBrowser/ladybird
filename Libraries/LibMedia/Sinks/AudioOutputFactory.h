/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibAudio/NullPlaybackStream.h>
#include <LibAudio/OutputSession.h>
#include <LibAudio/PlaybackStream.h>

namespace Media {

// Opens the audio outputs of one source of sound. A playback manager hands its factory to every sink it creates, and a
// sink opens another output with it after losing one.
class AudioOutputFactory final : public AtomicRefCounted<AudioOutputFactory> {
public:
    // The outputs play as part of the session.
    static NonnullRefPtr<AudioOutputFactory> create(NonnullRefPtr<Audio::OutputSession> session)
    {
        return adopt_ref(*new AudioOutputFactory([session = move(session)](Audio::OutputState initial_output_state, u32 target_latency_ms, Audio::PlaybackStream::AudioDataRequestCallback data_request_callback) {
            return Audio::PlaybackStream::create(initial_output_state, target_latency_ms, session, move(data_request_callback));
        }));
    }

    // The outputs discard what they are given, at the pace a device would play it.
    static NonnullRefPtr<AudioOutputFactory> create_null()
    {
        return adopt_ref(*new AudioOutputFactory([](Audio::OutputState initial_output_state, u32 target_latency_ms, Audio::PlaybackStream::AudioDataRequestCallback data_request_callback) {
            auto promise = Audio::PlaybackStream::CreatePromise::construct();
            promise->resolve(Audio::NullPlaybackStream::create(initial_output_state, target_latency_ms, move(data_request_callback)));
            return promise;
        }));
    }

    NonnullRefPtr<Audio::PlaybackStream::CreatePromise> open_output(Audio::OutputState initial_output_state, u32 target_latency_ms, Audio::PlaybackStream::AudioDataRequestCallback data_request_callback) const
    {
        return m_open_output(initial_output_state, target_latency_ms, move(data_request_callback));
    }

private:
    using OpenOutput = Function<NonnullRefPtr<Audio::PlaybackStream::CreatePromise>(Audio::OutputState, u32 target_latency_ms, Audio::PlaybackStream::AudioDataRequestCallback)>;

    explicit AudioOutputFactory(OpenOutput open_output)
        : m_open_output(move(open_output))
    {
    }

    OpenOutput m_open_output;
};

}
