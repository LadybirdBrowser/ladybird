/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibMedia/Audio/AudioClientEndpoint.h>
#include <LibMedia/Audio/AudioServerEndpoint.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Export.h>
#include <LibThreading/Thread.h>

namespace Audio {

class RemotePlaybackStream;

// A process's connection to the AudioServer, with the one pump thread that keeps every stream's ring filled.
class MEDIA_API ClientConnection final
    : public IPC::ConnectionToServer<AudioClientEndpoint, AudioServerEndpoint>
    , public AudioClientEndpoint {
    C_OBJECT_ABSTRACT(ClientConnection);

public:
    using TransportFactory = Function<ErrorOr<NonnullOwnPtr<IPC::Transport>>()>;
    // The factory reaches the Browser, which brokers the connection.
    static void set_transport_factory(TransportFactory);
    static bool has_transport_factory();
    // The process's connection, established on first use and replaced after it dies.
    static ErrorOr<NonnullRefPtr<ClientConnection>> acquire();

    explicit ClientConnection(NonnullOwnPtr<IPC::Transport>);
    virtual ~ClientConnection() override;

    Core::EventLoop& event_loop() { return m_event_loop; }

    // Resolves once the server reports the device format; rejects if it cannot open a device or dies first.
    NonnullRefPtr<PlaybackStream::CreatePromise> create_stream(OutputState, u32 target_latency_ms, PlaybackStream::AudioDataRequestCallback);

    enum class StreamRequest : u8 {
        Resume,
        Drain,
        Discard,
    };
    // Control thread. on_complete runs here when the server reports the request done or the connection dies, with the
    // stream if it is still alive; it holds no reference to the stream.
    using RequestCompletion = Function<void(RemotePlaybackStream*)>;
    void send_request(Badge<RemotePlaybackStream>, u64 stream_id, StreamRequest, RequestCompletion on_complete);
    void stream_destroyed(Badge<RemotePlaybackStream>, u64 stream_id);
    void register_stream(Badge<RemotePlaybackStream>, RemotePlaybackStream&);
    void unregister_stream(Badge<RemotePlaybackStream>, RemotePlaybackStream&);
    // Any thread.
    void wake_pump();

private:
    virtual void die() override;

    virtual void stream_created(u64 stream_id, SampleSpecification sample_specification, u32 device_latency_ms) override;
    virtual void stream_creation_failed(u64 stream_id) override;
    virtual void stream_request_completed(u64 stream_id, u64 request_id) override;

    void pump_thread_main();
    RefPtr<RemotePlaybackStream> find_live_stream(u64 stream_id);

    struct PendingRequest {
        u64 stream_id { 0 };
        RequestCompletion on_complete;
    };

    struct PendingStream {
        NonnullRefPtr<PlaybackStream::CreatePromise> promise;
        PlaybackStream::AudioDataRequestCallback callback;
        OutputState initial_state { OutputState::Suspended };
        u32 target_latency_ms { 0 };
    };

    Core::EventLoop& m_event_loop;
    u64 m_next_stream_id { 1 };
    u64 m_next_request_id { 1 };
    HashMap<u64, PendingStream> m_pending_streams;
    HashMap<u64, PendingRequest> m_pending_requests;
    bool m_is_dead { false };

    // Streams register themselves for their lifetime; the pump takes a reference only if one is still alive.
    Mutex m_pump_mutex;
    ConditionVariable m_pump_condition { m_pump_mutex };
    Vector<RemotePlaybackStream*> m_streams;
    bool m_pump_should_exit { false };
    bool m_pump_woken { false };
    AK::Duration m_pump_period { AK::Duration::from_milliseconds(25) };
    RefPtr<Threading::Thread> m_pump_thread;
};

}
