/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Memory.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Weakable.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <thread>

namespace AudioServer {

class SingleSinkSessionClient final
    : public RefCounted<SingleSinkSessionClient>
    , public Weakable<SingleSinkSessionClient> {
public:
    static constexpr u32 default_target_latency_ms = 50;

    using OutputSinkReadyHandler = Function<void(AudioServer::OutputSink const&)>;
    using OutputSinkFailedHandler = Function<void(u64 session_id, ByteString const& error)>;

    static ErrorOr<NonnullRefPtr<SingleSinkSessionClient>> try_create(RefPtr<SessionClientOfAudioServer> session_client = {});
    ~SingleSinkSessionClient();

    // Request one output session together with a sink for the selected device.
    ErrorOr<void> request_output_sink(
        OutputSinkReadyHandler on_ready,
        OutputSinkFailedHandler on_failed = {},
        AudioServer::DeviceHandle device_handle = 0,
        u32 target_latency_ms = default_target_latency_ms);

    ErrorOr<u64> destroy_output_sink(
        Function<void()> on_success = {},
        SessionClientOfAudioServer::RequestErrorHandler on_error = {});
    ErrorOr<void> release_output_sink_if_any();

    ErrorOr<u64> set_output_sink_volume(
        double volume,
        Function<void()> on_success = {},
        SessionClientOfAudioServer::RequestErrorHandler on_error = {});

    Optional<u64> active_session_id() const;

private:
    explicit SingleSinkSessionClient(NonnullRefPtr<SessionClientOfAudioServer>);
    void verify_thread_affinity() const { VERIFY(m_creation_thread == std::this_thread::get_id()); }

    NonnullRefPtr<SessionClientOfAudioServer> m_session_client;
    u64 m_output_sink_id { 0 };
    Optional<u64> m_active_session_id;
    Optional<u64> m_pending_session_id;
    Optional<AudioServer::DeviceHandle> m_device_handle;
    bool m_create_request_in_flight { false };
    OutputSinkReadyHandler m_on_output_sink_ready;
    OutputSinkFailedHandler m_on_output_sink_failed;
    std::thread::id m_creation_thread { std::this_thread::get_id() };
};

}
