/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Audio/PlaybackStream.h>

namespace Audio {

// A virtual audio output device which pulls audio at wall-clock pace and discards it. This keeps media pipelines
// running without requiring an audio server or hardware output device.
class MEDIA_API NullPlaybackStream final : public PlaybackStream {
public:
    static NonnullRefPtr<PlaybackStream> create(OutputState initial_output_state, u32 target_latency_ms, AudioDataRequestCallback);

    virtual SampleSpecification sample_specification() const override;
    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;
    virtual void notify_data_available() override;
    virtual AK::Duration total_time_played() const override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;

private:
    class State;

    explicit NullPlaybackStream(NonnullRefPtr<State>);
    virtual ~NullPlaybackStream() override;

    NonnullRefPtr<State> m_state;
};

}
