/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCamera/ClientConnection.h>
#include <LibCamera/Devices.h>

namespace Camera {

Devices& Devices::the()
{
    static NeverDestroyed<Devices> devices;
    return *devices;
}

void Devices::ensure_watching()
{
    if (m_watching)
        return;

    // A process with no server to reach has no cameras, and neither has one whose server won't start.
    auto connection = ClientConnection::has_transport_factory() ? ClientConnection::acquire() : Error::from_string_literal("No camera server");
    if (connection.is_error()) {
        report({});
        return;
    }

    // A list kept from a server that went away is not the new server's.
    m_has_device_list = false;
    m_watching = true;
    connection.value()->watch_devices([this](ErrorOr<Vector<DeviceInfo>> devices) {
        if (devices.is_error()) {
            // The cameras went with the server; the next to look for them starts another.
            m_watching = false;
            report({});
            return;
        }
        report(devices.release_value());
    });
}

bool Devices::request_fresh_device_list()
{
    if (!m_watching)
        return false;
    auto connection = ClientConnection::acquire();
    if (connection.is_error())
        return false;
    connection.value()->request_device_list();
    return true;
}

void Devices::report(Vector<DeviceInfo> devices)
{
    m_devices = move(devices);
    m_has_device_list = true;

    // A listener may add or remove others as it hears.
    Vector<ListenerId> listener_ids;
    listener_ids.ensure_capacity(m_listeners.size());
    for (auto const& listener : m_listeners)
        listener_ids.append(listener.key);
    for (auto listener_id : listener_ids) {
        // NB: The listener is kept alive by its own reference, as the map may grow or let go of it while it runs.
        if (auto listener = m_listeners.get(listener_id); listener.has_value())
            NonnullRefPtr { *listener.value() }->callback();
    }
}

Devices::ListenerId Devices::add_devices_changed_listener(Function<void()> listener)
{
    auto listener_id = m_next_listener_id++;
    m_listeners.set(listener_id, adopt_ref(*new Listener(move(listener))));
    return listener_id;
}

void Devices::remove_devices_changed_listener(ListenerId listener_id)
{
    m_listeners.remove(listener_id);
}

}
