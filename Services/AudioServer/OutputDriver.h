/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ThreadedPromise.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>

namespace AudioServer {

enum class OutputState : u8 {
    Playing,
    Suspended,
};

struct TimingInfo {
    static constexpr u32 s_magic = 0x4154494Du; // ATIM

    u32 magic { 0 };
    AK_CACHE_ALIGNED Atomic<u32, AK::MemoryOrder::memory_order_seq_cst> sequence { 0 };
    AK_CACHE_ALIGNED Atomic<u64, AK::MemoryOrder::memory_order_seq_cst> device_played_frames { 0 };
    AK_CACHE_ALIGNED Atomic<u64, AK::MemoryOrder::memory_order_seq_cst> ring_read_frames { 0 };
    AK_CACHE_ALIGNED Atomic<u64, AK::MemoryOrder::memory_order_seq_cst> server_monotonic_ns { 0 };
    AK_CACHE_ALIGNED Atomic<u64, AK::MemoryOrder::memory_order_seq_cst> underrun_count { 0 };
};

struct OutputSinkTransport {
    u64 session_id { 0 };
    u32 sample_rate { 0 };
    u32 channel_count { 0 };
    Audio::ChannelMap channel_layout;
    SharedCircularBuffer sample_ring_buffer;
    Core::AnonymousBuffer timing_buffer;
};

class OutputDriver {
public:
    using SampleSpecificationCallback = Function<void(Audio::SampleSpecification)>;
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

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, AudioServer::OutputSinkTransport const&);

template<>
ErrorOr<AudioServer::OutputSinkTransport> decode(Decoder&);

}
