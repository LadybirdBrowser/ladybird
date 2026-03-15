/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/Random.h>
#include <AudioServer/Debug.h>
#include <AudioServer/Server.h>
#include <AudioServer/SessionConnection.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibThreading/Mutex.h>

namespace AudioServer {

Server& Server::the()
{
    static Server s_server;
    return s_server;
}

int Server::allocate_session_client_id()
{
    return m_session_client_ids.allocate();
}

void Server::release_session_client_id(int client_id)
{
    m_session_client_ids.deallocate(client_id);
}

void Server::register_session_connection(SessionConnection& connection)
{
    if (!m_control_event_queue)
        m_control_event_queue = &Core::ThreadEventQueue::current();
    m_session_connections.set(connection.client_id(), &connection);
}

void Server::unregister_session_connection(int client_id)
{
    m_session_connections.remove(client_id);
}

void Server::revoke_grant_on_all_sessions(ByteString const& grant_id)
{
    for (auto const& it : m_session_connections) {
        auto const& connection = it.value;
        if (!connection)
            continue;
        connection->stop_all_streams_for_grant_revocation(grant_id);
    }
}

u64 Server::allocate_output_sink_id()
{
    return m_next_output_sink_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

u64 Server::allocate_input_stream_id()
{
    return m_next_input_stream_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

OutputStream& Server::output_stream(DeviceHandle device_handle)
{
    Threading::MutexLocker locker(m_output_streams_mutex);

    auto existing_stream = m_output_streams.get(device_handle);
    if (existing_stream.has_value())
        return *existing_stream.value();

    auto new_stream = MUST(adopt_nonnull_own_or_enomem(new (nothrow) OutputStream(device_handle)));
    OutputStream* new_stream_ptr = new_stream.ptr();
    m_output_streams.set(device_handle, move(new_stream));
    return *new_stream_ptr;
}

void Server::ensure_output_device_started(DeviceHandle device_handle, Core::ThreadEventQueue& control_event_queue, u32 target_latency_ms)
{
    output_stream(device_handle).ensure_started(control_event_queue, target_latency_ms);
}

void Server::when_output_device_ready(DeviceHandle device_handle, Function<void()> callback)
{
    output_stream(device_handle).when_ready(move(callback));
}

void Server::register_output_producer(DeviceHandle device_handle, u64 producer_id, AudioServer::SharedCircularBuffer ring, Core::AnonymousBuffer timing_buffer, size_t bytes_per_frame)
{
    output_stream(device_handle).register_producer(producer_id, move(ring), move(timing_buffer), bytes_per_frame);
}

void Server::unregister_output_producer(DeviceHandle device_handle, u64 producer_id)
{
    output_stream(device_handle).unregister_producer(producer_id);
}

Vector<DeviceInfo> Server::enumerate_devices()
{
    Threading::MutexLocker locker(m_device_cache_mutex);
    if (m_devices.is_empty())
        m_devices = enumerate_platform_devices();
    ensure_channel_layouts(m_devices);
    return m_devices;
}

void Server::update_devices()
{
    Vector<DeviceInfo> refreshed_devices = enumerate_platform_devices();
    ensure_channel_layouts(refreshed_devices);
    {
        Threading::MutexLocker locker(m_device_cache_mutex);
        if (m_devices == refreshed_devices)
            return;

        m_devices = move(refreshed_devices);
    }
    auto* control_event_queue = m_control_event_queue;
    if (!control_event_queue)
        return;
    control_event_queue->deferred_invoke([this] {
        for (auto const& it : m_session_connections) {
            auto const& connection = it.value;
            if (!connection)
                continue;
            connection->notify_devices_changed();
        }
    });
}

DeviceHandle Server::make_device_handle(u64 backend_handle, DeviceInfo::Type type)
{
    u64 type_bit = (type == DeviceInfo::Type::Output) ? 1u : 0u;
    return static_cast<DeviceHandle>((backend_handle << 1) | type_bit);
}

u64 Server::device_handle_to_os_device_id(DeviceHandle device_handle)
{
    return static_cast<u64>(device_handle) >> 1;
}

static Optional<DeviceInfo> find_device_by_handle(Vector<DeviceInfo> const& devices, DeviceHandle device_handle)
{
    for (auto const& device : devices) {
        if (device.device_handle == device_handle)
            return device;
    }
    return {};
}

Optional<DeviceInfo> Server::get_device(DeviceHandle device_handle)
{
    return find_device_by_handle(enumerate_devices(), device_handle);
}

void Server::ensure_channel_layouts(Vector<DeviceInfo>& devices)
{
    for (auto& device : devices) {
        bool has_unknown_channel = false;
        for (int i = 0; i < device.channel_layout.channel_count(); i++) {
            if (device.channel_layout.channel_at(i) == Audio::Channel::Unknown)
                has_unknown_channel = true;
        }
        if (!has_unknown_channel && device.channel_layout.is_valid())
            continue;
        switch (device.channel_count) {
        case 1:
            device.channel_layout = Audio::ChannelMap::mono();
            continue;
        case 2:
            device.channel_layout = Audio::ChannelMap::stereo();
            continue;
        case 4:
            device.channel_layout = Audio::ChannelMap::quadrophonic();
            continue;
        case 6:
            device.channel_layout = Audio::ChannelMap::surround_5_1();
            continue;
        case 8:
            device.channel_layout = Audio::ChannelMap::surround_7_1();
            continue;
        default:
            continue;
        }
    }
}

ByteString Server::generate_dom_device_id(StringView kind, ByteString const& backend_persistent_id, u64 device_handle)
{
    // TODO: Follow mediacapture-main Best Practice 4 with private-keyed, origin-scoped, salted ids.
    // TODO: Rotate ids when persistent storage is cleared.
    u32 kind_hash = string_hash(kind.characters_without_null_termination(), kind.length());

    u32 backend_hash = 0;
    if (!backend_persistent_id.is_empty())
        backend_hash = string_hash(backend_persistent_id.characters(), backend_persistent_id.length(), kind_hash);

    u32 combined_hash = pair_int_hash(pair_int_hash(kind_hash, backend_hash), u64_hash(device_handle));
    return ByteString::formatted("{}-{:08x}", kind, combined_hash);
}

ByteString Server::generate_grant_id()
{
    Array<u8, 16> token_bytes {};
    fill_with_random(token_bytes.span());
    return encode_hex(token_bytes);
}

bool Server::is_grant_active(ByteString const& grant_id) const
{
    return m_grants.contains(grant_id);
}

bool Server::can_grant_use_mic(ByteString const& grant_id) const
{
    auto grant = m_grants.get(grant_id);
    return grant.has_value() && grant->can_use_mic;
}

ByteString Server::create_grant(ByteString origin, ByteString top_level_origin, bool can_use_mic)
{
    ByteString grant_id;
    do {
        grant_id = generate_grant_id();
    } while (m_grants.contains(grant_id));

    m_grants.set(grant_id, GrantRecord { .origin = move(origin), .top_level_origin = move(top_level_origin), .can_use_mic = can_use_mic });

    if (should_log_audio_server())
        dbgln("create_grant() -> {}", grant_id);

    return grant_id;
}

bool Server::revoke_grant(ByteString const& grant_id)
{
    bool removed = m_grants.remove(grant_id);
    if (!removed)
        return false;

    if (should_log_audio_server())
        dbgln("revoke_grant({})", grant_id);

    return true;
}

}
