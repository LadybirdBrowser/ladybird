/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibAudioServer/ToAudioServerFromSessionClientEndpoint.h>
#include <LibAudioServer/ToSessionClientFromAudioServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <thread>

namespace Audio {

class SessionClientOfAudioServer final
    : public IPC::ConnectionToServer<ToSessionClientFromAudioServerEndpoint, ToAudioServerFromSessionClientEndpoint>
    , public ToSessionClientFromAudioServerEndpoint {
    C_OBJECT_ABSTRACT(SessionClientOfAudioServer);

public:
    using InitTransport = Messages::ToAudioServerFromSessionClient::InitTransport;
    using RequestErrorHandler = Function<void(ByteString)>;

    using OutputSinkReady = Function<bool(OutputSink const&)>;
    using OutputSinkFailed = Function<void(u64 session_id, ByteString const& error)>;

    explicit SessionClientOfAudioServer(NonnullOwnPtr<IPC::Transport>);

    void set_grant_id(ByteString grant_id) { m_grant_id = move(grant_id); }
    ByteString const& grant_id() const { return m_grant_id; }

    static void set_default_client(RefPtr<SessionClientOfAudioServer>);
    static RefPtr<SessionClientOfAudioServer> default_client();

    ErrorOr<u64> get_devices(
        Function<void(Vector<DeviceInfo>)> on_success,
        RequestErrorHandler on_error = {});

    ErrorOr<u64> create_output_session(
        u32 target_latency_ms,
        Function<void(u64)> on_success,
        RequestErrorHandler on_error = {},
        DeviceHandle device_handle = 0,
        Optional<u64> output_sink_id = {});

    ErrorOr<u64> destroy_session(
        u64 session_id,
        Function<void()> on_success = {},
        RequestErrorHandler on_error = {});

    u64 add_output_sink(
        OutputSinkReady on_ready,
        OutputSinkFailed on_failed = {});

    void remove_output_sink(u64 sink_id);
    Function<void(OutputSink)> on_output_sink_ready;
    Function<void(u64 session_id, ByteString error)> on_output_sink_failed;
    Function<void()> on_devices_changed;

    ErrorOr<u64> create_input_stream(
        DeviceHandle device_handle,
        u64 capacity_frames,
        Function<void(InputStreamDescriptor)> on_success,
        RequestErrorHandler on_error = {});

    ErrorOr<u64> start_input_stream(
        u64 stream_id,
        Function<void()> on_success = {},
        RequestErrorHandler on_error = {});

    ErrorOr<u64> stop_input_stream(
        u64 stream_id,
        Function<void()> on_success = {},
        RequestErrorHandler on_error = {});
    ErrorOr<u64> destroy_input_stream(
        u64 stream_id,
        Function<void()> on_success = {},
        RequestErrorHandler on_error = {});

    ErrorOr<u64> set_output_sink_volume(
        u64 session_id,
        double volume,
        Function<void()> on_success = {},
        RequestErrorHandler on_error = {});

    Function<void()> on_death;

private:
    using PendingDeviceInfosResult = Function<void(Vector<DeviceInfo>)>;
    using PendingInputStreamDescriptorResult = Function<void(InputStreamDescriptor)>;
    using PendingVoidResult = Function<void()>;
    using PendingU64Result = Function<void(u64)>;
    using PendingOutputSinkOutcome = Variant<OutputSinkTransport, ByteString>;
    using PendingRequestResult = Variant<
        PendingDeviceInfosResult,
        PendingInputStreamDescriptorResult,
        PendingVoidResult,
        PendingU64Result>;

    struct PendingRequest {
        PendingRequestResult on_success;
        RequestErrorHandler on_error;
    };

    struct OutputSinkCallbacks {
        OutputSinkReady on_ready;
        OutputSinkFailed on_failed;
    };

    template<typename Callback>
    void store_pending_request(u64 request_id, Callback&& callback, RequestErrorHandler on_error)
    {
        verify_thread_affinity();
        m_pending_requests.set(request_id, PendingRequest {
                                               .on_success = PendingRequestResult { forward<Callback>(callback) },
                                               .on_error = move(on_error),
                                           });
    }

    template<typename ExpectedCallback, typename... Args>
    void dispatch_pending_request(u64 request_id, Args&&... args)
    {
        verify_thread_affinity();
        auto pending_request = m_pending_requests.take(request_id);
        if (!pending_request.has_value())
            return;
        if (auto* typed = pending_request->on_success.template get_pointer<ExpectedCallback>())
            (*typed)(forward<Args>(args)...);
    }
    void verify_thread_affinity() const { VERIFY(m_creation_thread == std::this_thread::get_id()); }
    void deliver_output_sink_ready(Optional<u64> sink_id, OutputSinkTransport);
    void deliver_output_sink_failed(u64 sink_id, u64 session_id, ByteString const& error);
    void complete_pending_request_error(u64 request_id, ByteString error);
    u64 next_request_id();

    void die() override;
    void did_get_devices(u64 request_id, Vector<DeviceInfo> devices) override;
    void did_create_input_stream(u64 request_id, InputStreamDescriptor descriptor) override;
    void did_start_input_stream(u64 request_id) override;
    void did_stop_input_stream(u64 request_id) override;
    void did_destroy_input_stream(u64 request_id) override;
    void did_set_output_sink_volume(u64 request_id) override;
    void did_create_session(u64 request_id, u64 session_id) override;
    void did_destroy_session(u64 request_id) override;
    void request_error(u64 request_id, ByteString error) override;

    void output_sink_ready(OutputSinkTransport output_sink_transport) override;
    void output_sink_failed(u64 session_id, ByteString error) override;
    void notify_devices_changed() override;

    ByteString m_grant_id;
    u64 m_next_request_id { 1 };
    HashMap<u64, PendingRequest> m_pending_requests;
    HashMap<u64, OutputSinkCallbacks> m_create_sink_callbacks;
    HashMap<u64, u64> m_pending_output_sink_requests;
    HashMap<u64, u64> m_output_sink_sessions;
    HashMap<u64, PendingOutputSinkOutcome> m_pending_output_sink_outcomes;
    u64 m_next_output_sink_id { 1 };
    std::thread::id m_creation_thread { std::this_thread::get_id() };
};

}
