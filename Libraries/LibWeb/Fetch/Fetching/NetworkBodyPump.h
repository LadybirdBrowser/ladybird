/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/RefCounted.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Forward.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibRequests/Request.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

// The producer half of a streamed response body: memory-cache accumulation, and the writes into the channel the
// stream pulls from.
class WEB_API NetworkBodyPump final : public RefCounted<NetworkBodyPump> {
public:
    GC_HOLDS_NO_CELL_EDGES;

    [[nodiscard]] static NonnullRefPtr<NetworkBodyPump> create(RefPtr<Infrastructure::FetchParams const>, Engine::FetchBodyDeliveryGate::NavigationHold, RefPtr<HTTP::MemoryCache>);

    ~NetworkBodyPump();

    // How many pumps made on this thread are alive.
    [[nodiscard]] static u32 live_count();

    [[nodiscard]] NonnullRefPtr<Engine::FetchByteChannel> const& channel() const { return m_channel; }
    [[nodiscard]] NonnullRefPtr<Engine::FetchBodyDeliveryGate> const& delivery_gate() const { return m_delivery_gate; }

    // The pump records the body's sizes in the response once the body completes.
    void set_response(NonnullRefPtr<Fetch::Infrastructure::Response>);

    void handle_network_data(Requests::ResponseData);
    void set_cached_response_body(Core::ImmutableBytes);

    // Each of these detaches the delivery gate before it closes, errors, cancels or transfers the body's channel.
    void handle_network_complete(Optional<i64> request_server_encoded_body_size = {});
    void handle_network_error(StringView error_message);
    void handle_network_teardown(Requests::Request::TeardownReason);

private:
    NetworkBodyPump(RefPtr<Infrastructure::FetchParams const>, NonnullRefPtr<Engine::FetchByteChannel>, NonnullRefPtr<Engine::FetchBodyDeliveryGate>, RefPtr<HTTP::MemoryCache>);

    // Drops a pending memory-cache entry that matches the response's request, for a body that will not finish arriving.
    void abandon_cache_entry(HTTP::MemoryCache::StoredResponse);
    void finalize_cache_entry();

    RefPtr<Infrastructure::FetchParams const> m_fetch_params;
    RefPtr<Fetch::Infrastructure::Response> m_response;

    NonnullRefPtr<Engine::FetchByteChannel> m_channel;
    NonnullRefPtr<Engine::FetchBodyDeliveryGate> m_delivery_gate;

    RefPtr<HTTP::MemoryCache> m_http_cache;

    // Whole-response buffer retained only when m_http_cache is non-null, for finalize_entry().
    ByteBuffer m_cache_buffer;
    Optional<Core::ImmutableBytes> m_cache_body;
    bool m_cache_body_replaces_network_buffer { false };
    bool m_cache_body_too_large_for_memory { false };

    u64 m_encoded_body_size { 0 };
    u64 m_decoded_body_size { 0 };
    bool m_network_complete { false };
};

}
