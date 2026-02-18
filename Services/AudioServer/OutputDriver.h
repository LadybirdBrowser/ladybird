/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibAudioServer/SampleSpecification.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ThreadedPromise.h>
#include <LibIPC/Forward.h>

namespace Audio {

enum class OutputState : u8 {
    Playing,
    Suspended,
};

class OutputDriver {
public:
    using SampleSpecificationCallback = Function<void(SampleSpecification)>;
    using AudioDataRequestCallback = Function<ReadonlySpan<float>(Span<float> buffer)>;

    virtual ~OutputDriver() = default;

    virtual void set_underrun_callback(Function<void()>) = 0;
    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() = 0;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() = 0;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() = 0;
    virtual AK::Duration device_time_played() const = 0;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) = 0;
};

ErrorOr<NonnullOwnPtr<OutputDriver>> create_platform_output_driver(DeviceHandle device_handle, OutputState initial_output_state, u32 target_latency_ms, OutputDriver::SampleSpecificationCallback&& sample_specification_callback, OutputDriver::AudioDataRequestCallback&& data_request_callback);

}
