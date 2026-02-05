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

class PlaybackStreamWASAPI final : public PlaybackStream {
public:
    static ErrorOr<NonnullRefPtr<PlaybackStream>> create(OutputState initial_output_state, u32 target_latency_ms, SampleSpecificationCallback&&, AudioDataRequestCallback&&);

    // The overrun callback must be realtime safe. The buffer size might be small.
    virtual void set_underrun_callback(Function<void()>) override;

    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;

    virtual AK::Duration total_time_played() const override;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;

private:
    struct AudioState;

    explicit PlaybackStreamWASAPI(NonnullRefPtr<AudioState>);

    static ALWAYS_INLINE AK::Duration total_time_played_with_com_initialized(AudioState& state);
    ~PlaybackStreamWASAPI();

    NonnullRefPtr<AudioState> m_state;
};

}
