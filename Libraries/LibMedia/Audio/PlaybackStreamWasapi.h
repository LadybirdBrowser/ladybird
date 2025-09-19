/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PlaybackStream.h"
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>

namespace Audio {

struct AudioState;

class PlaybackStreamWasapi final : public PlaybackStream {
public:
    static ErrorOr<NonnullRefPtr<PlaybackStream>> create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32 target_latency_ms, AudioDataRequestCallback&& data_request_callback);

    // The overrun callback must be realtime safe. The buffer size might be small.
    virtual void set_underrun_callback(Function<void()>) override;

    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;

    virtual ErrorOr<AK::Duration> total_time_played() override;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;

private:
    explicit PlaybackStreamWasapi(NonnullRefPtr<AudioState>);
    ~PlaybackStreamWasapi();

    NonnullRefPtr<AudioState> m_state;
};

}
