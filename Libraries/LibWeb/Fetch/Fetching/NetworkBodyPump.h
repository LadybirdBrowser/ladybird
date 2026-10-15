/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/CellAllocator.h>
#include <LibHTTP/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibRequests/Request.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

// The producer half of a streamed response body: memory-cache accumulation, and the writes into the channel the
// stream pulls from.
class NetworkBodyPump final : public JS::Cell {
    GC_CELL(NetworkBodyPump, JS::Cell);
    GC_DECLARE_ALLOCATOR(NetworkBodyPump);

public:
    virtual ~NetworkBodyPump() override;

    // The pump records the body's sizes in the response once the body completes.
    void set_response(NonnullRefPtr<Fetch::Infrastructure::Response>);

    void handle_network_data(Requests::ResponseData);
    void handle_network_complete(Optional<u64> encoded_body_size = {});
    void handle_network_error(StringView error_message);
    void set_cached_response_body(Core::ImmutableBytes);

    // Drops the response's pending memory-cache entry, for a body that will not finish arriving.
    void abandon_cache_entry();

private:
    NetworkBodyPump(RefPtr<Infrastructure::FetchParams const>, NonnullRefPtr<Engine::FetchByteChannel>, NonnullRefPtr<Engine::FetchBodyDeliveryGate>, RefPtr<HTTP::MemoryCache>);

    virtual void visit_edges(Visitor& visitor) override;

    void finalize_cache_entry();
    void update_response_body_info();

    RefPtr<Infrastructure::FetchParams const> m_fetch_params;
    // The controller of m_fetch_params, which the pump keeps alive with them.
    GC::Ptr<Infrastructure::FetchController> m_fetch_controller;
    RefPtr<Fetch::Infrastructure::Response> m_response;

    NonnullRefPtr<Engine::FetchByteChannel> m_channel;
    NonnullRefPtr<Engine::FetchBodyDeliveryGate> m_delivery_gate;

    RefPtr<HTTP::MemoryCache> m_http_cache;

    // Whole-response buffer retained only when m_http_cache is non-null, for finalize_entry().
    ByteBuffer m_cache_buffer;
    Optional<Core::ImmutableBytes> m_cache_body;
    bool m_cache_body_replaces_network_buffer { false };

    u64 m_encoded_body_size { 0 };
    u64 m_decoded_body_size { 0 };
    bool m_network_complete { false };
};

}
