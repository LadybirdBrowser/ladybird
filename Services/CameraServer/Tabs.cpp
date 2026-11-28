/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <CameraServer/ControlConnection.h>
#include <CameraServer/Tabs.h>
#include <LibIPC/Transport.h>

namespace CameraServer {

Tabs& Tabs::the()
{
    static NeverDestroyed<Tabs> instance;
    return *instance;
}

void Tabs::set_backend(NonnullOwnPtr<CameraBackend> backend)
{
    VERIFY(!m_backend);
    m_backend = move(backend);
}

void Tabs::set_control_connection(ControlConnection& connection)
{
    m_control_connection = connection.make_weak_ptr<ControlConnection>();
}

void Tabs::set_preferred_device(ByteString device_id)
{
    if (m_preferred_device_id == device_id)
        return;
    m_preferred_device_id = move(device_id);
    report_devices();
}

void Tabs::report_devices()
{
    if (!m_devices.has_value())
        return;
    auto devices = with_preferred_default(*m_devices, m_preferred_device_id);
    for (auto& [tab_id, connections] : m_tabs) {
        for (auto& [client_id, connection] : connections)
            connection->set_devices(devices);
    }
    if (auto control_connection = m_control_connection.strong_ref())
        control_connection->async_devices_changed(move(devices));
}

void Tabs::watch_devices()
{
    if (m_is_watching_devices)
        return;
    m_is_watching_devices = true;
    m_backend->watch_devices([this](Vector<Camera::DeviceInfo> devices) {
        m_devices = move(devices);
        report_devices();
    });
}

ErrorOr<IPC::TransportHandle> Tabs::connect_client(u64 tab_id)
{
    auto paired_transports = TRY(IPC::Transport::create_paired());
    auto handle = move(paired_transports.remote_handle);

    auto client_id = m_next_client_id++;
    auto connection = ServerConnection::construct(move(paired_transports.local), client_id, *m_backend, [tab_id](ByteString const& device_id, u64 page_label, Function<void(bool)> on_decided) {
        Tabs::the().request_capture_permission(tab_id, device_id, page_label, move(on_decided));
    });
    connection->set_capture_permitted(m_tabs_with_capture_permission.contains(tab_id));
    connection->on_death = [tab_id, client_id] {
        Tabs::the().connection_died(tab_id, client_id);
    };
    connection->on_capture_started = [tab_id](u64 page_label) {
        Tabs::the().capture_started(tab_id, page_label);
    };
    connection->on_capture_stopped = [tab_id](u64 page_label) {
        Tabs::the().capture_stopped(tab_id, page_label);
    };
    if (m_devices.has_value())
        connection->set_devices(with_preferred_default(*m_devices, m_preferred_device_id));
    m_tabs.ensure(tab_id).set(client_id, move(connection));

    // The platform is first asked for its cameras when there is somebody to tell.
    watch_devices();
    return handle;
}

void Tabs::set_capture_permission(u64 tab_id, bool granted)
{
    if (granted)
        m_tabs_with_capture_permission.set(tab_id);
    else
        m_tabs_with_capture_permission.remove(tab_id);
    auto connections = m_tabs.get(tab_id);
    if (!connections.has_value())
        return;
    for (auto& [client_id, connection] : *connections)
        connection->set_capture_permitted(granted);
}

void Tabs::request_capture_permission(u64 tab_id, ByteString const& device_id, u64 page_label, Function<void(bool)> on_decided)
{
    auto control_connection = m_control_connection.strong_ref();
    if (!control_connection) {
        on_decided(false);
        return;
    }
    auto request_id = m_next_capture_request_id++;
    m_pending_capture_decisions.set(request_id, move(on_decided));
    control_connection->async_capture_requested(tab_id, request_id, device_id, page_label);
}

void Tabs::capture_decided(u64 request_id, bool allowed)
{
    if (auto on_decided = m_pending_capture_decisions.take(request_id); on_decided.has_value())
        on_decided.value()(allowed);
}

void Tabs::capture_started(u64 tab_id, u64 page_label)
{
    auto& count = m_capturing_stream_counts.ensure(tab_id).ensure(page_label);
    if (++count != 1)
        return;
    if (auto control_connection = m_control_connection.strong_ref())
        control_connection->async_capture_activity_changed(tab_id, page_label, true);
}

void Tabs::capture_stopped(u64 tab_id, u64 page_label)
{
    auto labels = m_capturing_stream_counts.get(tab_id);
    if (!labels.has_value())
        return;
    auto count = labels->get(page_label);
    if (!count.has_value() || --*count != 0)
        return;
    labels->remove(page_label);
    if (labels->is_empty())
        m_capturing_stream_counts.remove(tab_id);
    if (auto control_connection = m_control_connection.strong_ref())
        control_connection->async_capture_activity_changed(tab_id, page_label, false);
}

void Tabs::connection_died(u64 tab_id, int client_id)
{
    auto connections = m_tabs.get(tab_id);
    if (!connections.has_value())
        return;
    connections->remove(client_id);
    if (connections->is_empty())
        m_tabs.remove(tab_id);
}

}
