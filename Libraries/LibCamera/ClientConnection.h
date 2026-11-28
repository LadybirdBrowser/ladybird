/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibCamera/CameraClientEndpoint.h>
#include <LibCamera/CameraServerEndpoint.h>
#include <LibCamera/CaptureStream.h>
#include <LibCamera/DeviceInfo.h>
#include <LibIPC/ConnectionToServer.h>

namespace Camera {

// A process's connection to the CameraServer, which opens the cameras this process may not.
class ClientConnection final
    : public IPC::ConnectionToServer<CameraClientEndpoint, CameraServerEndpoint>
    , public CameraClientEndpoint {
    C_OBJECT_ABSTRACT(ClientConnection);

public:
    using TransportFactory = Function<ErrorOr<NonnullOwnPtr<IPC::Transport>>()>;
    // The factory reaches the Browser, which brokers the connection.
    static void set_transport_factory(TransportFactory);
    static bool has_transport_factory();
    // The process's connection, established on first use and replaced after it dies.
    static ErrorOr<NonnullRefPtr<ClientConnection>> acquire();
    static size_t capture_stream_count_for_testing();

    explicit ClientConnection(NonnullOwnPtr<IPC::Transport>);
    virtual ~ClientConnection() override;

    // Reports the cameras once a list is available and again on every change; an error says the server is gone, and is
    // the last report.
    using DeviceListCallback = Function<void(ErrorOr<Vector<DeviceInfo>>)>;
    void watch_devices(DeviceListCallback);
    // Has the server report its list again.
    void request_device_list();

    NonnullRefPtr<CaptureStream::CreatePromise> create_capture_stream(StringView device_id, u64 page_label, StreamRequest, CaptureStream::FrameCallback);
    void capture_stream_destroyed(Badge<CaptureStream>, u64 stream_id);

private:
    virtual void die() override;

    virtual void devices_changed(Vector<DeviceInfo> devices) override;
    virtual void capture_stream_created(u64 stream_id, u32 width, u32 height, u32 frame_rate, Vector<Core::AnonymousBuffer> buffers) override;
    virtual void capture_stream_creation_failed(u64 stream_id, CaptureFailure) override;
    virtual void capture_stream_closed(u64 stream_id) override;
    virtual void capture_stream_ended(u64 stream_id) override;
    virtual void frame_captured(u64 stream_id, u32 buffer_index, i64 timestamp_in_microseconds) override;

    struct PendingStream {
        NonnullRefPtr<CaptureStream::CreatePromise> promise;
        CaptureStream::FrameCallback callback;
    };

    void end_stream(u64 stream_id);

    HashMap<u64, PendingStream> m_pending_streams;
    // The streams hold the connection, not the other way around; each leaves this as it is destroyed.
    HashMap<u64, CaptureStream*> m_streams;
    u64 m_next_stream_id { 1 };
    DeviceListCallback m_on_device_list;
    bool m_is_dead { false };
};

}
