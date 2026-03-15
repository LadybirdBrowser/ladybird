/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/StdLibExtras.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AudioServer/InputStream.h>
#include <AudioServer/OutputDriver.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibAudioServer/ToAudioServerFromSessionClientEndpoint.h>
#include <LibAudioServer/ToSessionClientFromAudioServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <LibThreading/Mutex.h>
#include <thread>

namespace AudioServer {

class SessionClientOfAudioServer final
    : public IPC::ConnectionToServer<ToSessionClientFromAudioServerEndpoint, ToAudioServerFromSessionClientEndpoint>
    , public ToSessionClientFromAudioServerEndpoint {
    C_OBJECT_ABSTRACT(SessionClientOfAudioServer);

public:
    using InitTransport = Messages::ToAudioServerFromSessionClient::InitTransport;
    using RequestErrorHandler = Function<void(ByteString)>;

    using OutputSinkReady = Function<bool(AudioServer::OutputSink const&)>;
    using OutputSinkFailed = Function<void(u64 session_id, ByteString const& error)>;

    explicit SessionClientOfAudioServer(NonnullOwnPtr<IPC::Transport>);

    void set_grant_id(ByteString grant_id) { m_grant_id = move(grant_id); }
    ByteString const& grant_id() const { return m_grant_id; }

    static void set_default_client(RefPtr<SessionClientOfAudioServer>);
    static RefPtr<SessionClientOfAudioServer> default_client();

    ErrorOr<u64> get_devices(
        Function<void(Vector<AudioServer::DeviceInfo>)> on_success,
        RequestErrorHandler on_error = {});

    ErrorOr<u64> create_session(
        u32 target_latency_ms,
        Function<void(u64)> on_success,
        RequestErrorHandler on_error = {},
        AudioServer::DeviceHandle device_handle = 0);

    ErrorOr<u64> destroy_session(
        u64 session_id,
        Function<void()> on_success = {},
        RequestErrorHandler on_error = {});

    u64 add_output_sink(
        OutputSinkReady on_ready,
        OutputSinkFailed on_failed = {});

    void remove_output_sink(u64 sink_id);
    Function<void(AudioServer::OutputSink)> on_output_sink_ready;
    Function<void(u64 session_id, ByteString error)> on_output_sink_failed;
    Function<void()> on_devices_changed;

    ErrorOr<u64> create_input_stream(
        AudioServer::DeviceHandle device_handle,
        u64 capacity_frames,
        Function<void(AudioServer::InputStreamDescriptor)> on_success,
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
    void verify_thread_affinity() const { VERIFY(m_creation_thread == std::this_thread::get_id()); }
    u64 next_request_token();
    void die() override;

    using PendingDeviceInfosResult = Function<void(Vector<AudioServer::DeviceInfo>)>;
    using PendingInputStreamDescriptorResult = Function<void(AudioServer::InputStreamDescriptor)>;
    using PendingVoidResult = Function<void()>;
    using PendingU64Result = Function<void(u64)>;
    using PendingRequestResult = Variant<
        PendingDeviceInfosResult,
        PendingInputStreamDescriptorResult,
        PendingVoidResult,
        PendingU64Result>;

    template<typename Callback>
    void store_pending_request(u64 request_token, Callback&& callback, RequestErrorHandler on_error)
    {
        verify_thread_affinity();
        if (on_error)
            m_pending_request_errors.set(request_token, move(on_error));
        m_pending_request_results.set(request_token, PendingRequestResult { forward<Callback>(callback) });
    }

    template<typename ExpectedCallback, typename... Args>
    void dispatch_pending_request(u64 request_token, Args&&... args)
    {
        verify_thread_affinity();
        auto callback = m_pending_request_results.take(request_token);
        m_pending_request_errors.remove(request_token);
        if (!callback.has_value())
            return;
        if (auto* typed = callback->template get_pointer<ExpectedCallback>())
            (*typed)(forward<Args>(args)...);
    }

    void complete_pending_request_error(u64 request_token, ByteString error);

    void did_get_devices(u64 request_token, Vector<AudioServer::DeviceInfo> devices) override;
    void did_create_input_stream(u64 request_token, AudioServer::InputStreamDescriptor descriptor) override;
    void did_start_input_stream(u64 request_token) override;
    void did_stop_input_stream(u64 request_token) override;
    void did_destroy_input_stream(u64 request_token) override;
    void did_set_output_sink_volume(u64 request_token) override;
    void did_create_session(u64 request_token, u64 session_id) override;
    void did_destroy_session(u64 request_token) override;
    void request_error(u64 request_token, ByteString error) override;

    void output_sink_ready(AudioServer::OutputSinkTransport output_sink_transport) override;
    void output_sink_failed(u64 session_id, ByteString error) override;
    void notify_devices_changed() override;

    ByteString m_grant_id;
    u64 m_next_request_token { 1 };
    HashMap<u64, Function<void(ByteString)>> m_pending_request_errors;
    HashMap<u64, PendingRequestResult> m_pending_request_results;

    struct OutputSinkCallbacks {
        OutputSinkReady on_ready;
        OutputSinkFailed on_failed;
    };
    HashMap<u64, OutputSinkCallbacks> m_create_sink_callbacks;
    u64 m_next_output_sink_id { 1 };
    std::thread::id m_creation_thread { std::this_thread::get_id() };
};

}
