/*
 * Copyright (c) 2024, Olekoop <mlglol360xd@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PlaybackStream.h"
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>

namespace Audio {

class PlaybackStreamOboe final : public PlaybackStream {
public:
    static ErrorOr<NonnullRefPtr<PlaybackStream>> create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32 target_latency_ms, AudioDataRequestCallback&& data_request_callback);

    virtual void set_underrun_callback(Function<void()>) override;

    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;

    virtual ErrorOr<AK::Duration> total_time_played() override;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;

private:
    class Storage;
    explicit PlaybackStreamOboe(NonnullRefPtr<Storage>);
    ~PlaybackStreamOboe();
    RefPtr<Storage> m_storage;
};

}
