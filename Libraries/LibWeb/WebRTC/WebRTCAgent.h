/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWebRTCClient/Client.h>

namespace Web::WebRTC {

class RTCDataChannel;
class RTCPeerConnection;

class WebRTCAgent {
    AK_MAKE_NONCOPYABLE(WebRTCAgent);
    AK_MAKE_NONMOVABLE(WebRTCAgent);
    AK_ALLOC_WITH_KMALLOC;

public:
    static WebRTCAgent& the();

    using TransportFactory = Function<ErrorOr<NonnullOwnPtr<IPC::Transport>>()>;
    // The factory reaches the Browser, which launches a helper for this process.
    WEB_API static void set_transport_factory(TransportFactory);

    bool is_ready();
    WebRTCClient::Client* client();
    WebRTCClient::Client* existing_client() { return m_client.ptr(); }
    void close_connections_for_global(DOM::EventTarget&);

    u64 next_pc_id() { return ++m_next_pc_id; }
    u64 next_request_id() { return ++m_next_request_id; }
    u64 next_sender_id() { return ++m_next_sender_id; }
    u64 next_channel_id() { return ++m_next_channel_id; }
    // Start above the small SSRC values webrtc-rs tends to allocate to avoid collisions.
    u32 next_ssrc() { return ++m_next_ssrc; }

    void register_peer_connection(u64 pc_id, GC::Ref<RTCPeerConnection>);
    void unregister_peer_connection(u64 pc_id);
    GC::Ptr<RTCPeerConnection> find_peer_connection(u64 pc_id);

    void register_data_channel(u64 channel_id, GC::Ref<RTCDataChannel>);
    void unregister_data_channel(u64 channel_id);

private:
    WebRTCAgent() = default;

    void ensure_client();
    void wire_event_handlers();

    RefPtr<WebRTCClient::Client> m_client;
    bool m_attempted_to_launch { false };

    HashMap<u64, GC::Root<RTCPeerConnection>> m_peer_connections;
    HashMap<u64, GC::Root<RTCDataChannel>> m_data_channels;

    u64 m_next_pc_id { 0 };
    u64 m_next_request_id { 0 };
    u64 m_next_sender_id { 0 };
    u64 m_next_channel_id { 0 };
    u32 m_next_ssrc { 0xa0000000 };
};

}
