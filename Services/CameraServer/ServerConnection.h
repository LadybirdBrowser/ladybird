/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <CameraServer/CameraBackend.h>
#include <CameraServer/CaptureSession.h>
#include <LibCamera/CameraClientEndpoint.h>
#include <LibCamera/CameraServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>

namespace CameraServer {

class ServerConnection final
    : public IPC::ConnectionFromClient<CameraClientEndpoint, CameraServerEndpoint> {
    C_OBJECT(ServerConnection);

public:
    // A client may hold this many streams, opening or open.
    static constexpr size_t MAXIMUM_STREAMS = 8;

    // Asks the Browser whether the client's tab may capture from the camera; the answer comes once, on this thread.
    using CapturePermissionRequest = Function<void(ByteString const& device_id, u64 page_label, Function<void(bool allowed)> on_decided)>;

    virtual ~ServerConnection() override;

    // Runs when the client disconnects, after its streams are closed.
    Function<void()> on_death;
    // A capture stream counts from the moment the Browser lets it capture until it is gone, however it goes.
    Function<void(u64 page_label)> on_capture_started;
    Function<void(u64 page_label)> on_capture_stopped;

    // Whether the Browser says the client's tab holds the camera permission. Without it the client learns of the
    // cameras by their existence alone, and the capture streams it has are closed.
    void set_capture_permitted(bool);

    // The cameras there are, once they are known and whenever they change.
    void set_devices(Vector<Camera::DeviceInfo> const&);

private:
    ServerConnection(NonnullOwnPtr<IPC::Transport>, int client_id, CameraBackend&, CapturePermissionRequest);

    virtual void die() override;

    virtual void watch_devices() override;
    virtual void create_capture_stream(u64 stream_id, ByteString device_id, u64 page_label, Camera::StreamRequest request) override;
    virtual void destroy_capture_stream(u64 stream_id) override;
    virtual void release_frame(u64 stream_id, u32 buffer_index) override;

    // No session while the Browser is being asked.
    struct Stream {
        // Tells this stream from one the client later makes under the same ID.
        u64 generation { 0 };
        u64 page_label { 0 };
        bool may_capture { false };
        // Whether the client has been told the camera is open.
        bool is_open { false };
        RefPtr<CaptureSession> session;
        OwnPtr<CameraStream> camera;
    };

    void send_device_list();
    void capture_permission_decided(u64 stream_id, u64 generation, ByteString const& device_id, Camera::StreamRequest, bool allowed);
    void stream_gone(Stream&);
    void close_streams();

    CameraBackend& m_backend;
    CapturePermissionRequest m_request_capture_permission;
    HashMap<u64, Stream> m_streams;
    u64 m_next_stream_generation { 1 };
    bool m_capture_permitted { false };

    Optional<Vector<Camera::DeviceInfo>> m_devices;
    bool m_client_watches_devices { false };
};

}
