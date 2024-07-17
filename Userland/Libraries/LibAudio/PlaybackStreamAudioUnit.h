/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <LibAudio/PlaybackStream.h>

namespace Audio {

class AudioState;

class PlaybackStreamAudioUnit final : public PlaybackStream {
public:
    static ErrorOr<NonnullRefPtr<PlaybackStream>> create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32 target_latency_ms, AudioDataRequestCallback&& data_request_callback);

    virtual void set_underrun_callback(Function<void()>) override;

    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;

    virtual ErrorOr<AK::Duration> total_time_played() override;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;

private:
    explicit PlaybackStreamAudioUnit(NonnullRefPtr<AudioState>);
    ~PlaybackStreamAudioUnit();

    NonnullRefPtr<AudioState> m_state;
};

}
