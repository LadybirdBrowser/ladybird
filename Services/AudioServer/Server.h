/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/IDAllocator.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AudioServer/OutputStream.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibThreading/Mutex.h>

namespace Core {

class ThreadEventQueue;

}

namespace AudioServer {

class SessionConnection;

class Server final {
public:
    enum class GrantKind : u8 {
        Output = 1,
        InputMicrophone = 2,
    };

    static Server& the();

    int allocate_session_client_id();
    void release_session_client_id(int);
    void register_session_connection(SessionConnection&);
    void unregister_session_connection(int client_id);
    void revoke_grant_on_all_sessions(ByteString const& grant_id);

    u64 allocate_output_sink_id();
    u64 allocate_input_stream_id();

    void ensure_output_device_started(DeviceHandle device_handle, Core::ThreadEventQueue& control_event_queue, u32 target_latency_ms);
    void when_output_device_ready(DeviceHandle device_handle, Function<void()>);
    void register_output_producer(DeviceHandle device_handle, u64 producer_id, AudioServer::SharedCircularBuffer ring, Core::AnonymousBuffer timing_buffer, size_t bytes_per_frame);
    void unregister_output_producer(DeviceHandle device_handle, u64 producer_id);

    Vector<DeviceInfo> enumerate_devices();
    void update_devices();
    Optional<DeviceInfo> get_device(DeviceHandle device_handle);

    ByteString create_grant(ByteString origin, ByteString top_level_origin, bool can_use_mic);
    bool revoke_grant(ByteString const& grant_id);
    bool is_grant_active(ByteString const& grant_id) const;
    bool can_grant_use_mic(ByteString const& grant_id) const;

    static Audio::ChannelMap common_channel_layout(u32 channel_count);
    static u64 device_handle_to_os_device_id(DeviceHandle device_handle);

private:
    static ByteString generate_dom_device_id(StringView kind, ByteString const& backend_persistent_id, u64 device_handle);
    static DeviceHandle make_device_handle(u64 backend_handle, DeviceInfo::Type type);
    static void ensure_channel_layouts(Vector<DeviceInfo>& devices);
    Server() = default;

    struct GrantRecord {
        ByteString origin;
        ByteString top_level_origin;
        bool can_use_mic { false };
    };

    static ByteString generate_grant_id();
    static Vector<DeviceInfo> enumerate_platform_devices();

    Core::ThreadEventQueue* m_control_event_queue { nullptr };
    IDAllocator m_session_client_ids;
    HashMap<int, RefPtr<SessionConnection>> m_session_connections;
    HashMap<DeviceHandle, NonnullOwnPtr<OutputStream>> m_output_streams;
    HashMap<ByteString, GrantRecord> m_grants;
    Atomic<u64> m_next_output_sink_id { 1 };
    Atomic<u64> m_next_input_stream_id { 1 };
    Threading::Mutex m_output_streams_mutex;
    Threading::Mutex m_device_cache_mutex;
    Vector<DeviceInfo> m_devices;
    OutputStream& output_stream(DeviceHandle device_handle);
};

}
