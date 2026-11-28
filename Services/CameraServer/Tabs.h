/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/WeakPtr.h>
#include <CameraServer/CameraBackend.h>
#include <CameraServer/ServerConnection.h>
#include <LibIPC/TransportHandle.h>

namespace CameraServer {

class ControlConnection;

// The tabs with connected client processes, and the Browser's word on whether each one's pages hold the camera
// permission.
class Tabs {
public:
    static Tabs& the();

    void set_backend(NonnullOwnPtr<CameraBackend>);
    // The Browser rules on every capture request, and is asked over this connection.
    void set_control_connection(ControlConnection&);

    ErrorOr<IPC::TransportHandle> connect_client(u64 tab_id);
    // Whether the tab's pages hold the permission: its clients, present and future, learn the cameras' names only with
    // it, and the capture streams they have are closed without it.
    void set_capture_permission(u64 tab_id, bool granted);
    void capture_decided(u64 request_id, bool allowed);
    // The camera every tab's clients take for the default one, while it is there.
    void set_preferred_device(ByteString device_id);

private:
    void report_devices();
    void watch_devices();
    void connection_died(u64 tab_id, int client_id);
    void request_capture_permission(u64 tab_id, ByteString const& device_id, u64 page_label, Function<void(bool)> on_decided);
    // Notify the Browser when a label's first permitted stream starts or its last stream stops.
    void capture_started(u64 tab_id, u64 page_label);
    void capture_stopped(u64 tab_id, u64 page_label);

    OwnPtr<CameraBackend> m_backend;
    bool m_is_watching_devices { false };
    // As the backend gave them, before the user's preference.
    Optional<Vector<Camera::DeviceInfo>> m_devices;
    ByteString m_preferred_device_id;

    HashMap<u64, HashMap<int, NonnullRefPtr<ServerConnection>>> m_tabs;
    HashTable<u64> m_tabs_with_capture_permission;
    int m_next_client_id { 1 };

    WeakPtr<ControlConnection> m_control_connection;
    HashMap<u64, Function<void(bool)>> m_pending_capture_decisions;
    u64 m_next_capture_request_id { 1 };
    HashMap<u64, HashMap<u64, size_t>> m_capturing_stream_counts;
};

}
