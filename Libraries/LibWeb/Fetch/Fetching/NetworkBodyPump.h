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

// The producer half of a streamed response body: sniff capture, memory-cache accumulation, and the writes into
// the channel the stream pulls from.
class NetworkBodyPump final : public JS::Cell {
    GC_CELL(NetworkBodyPump, JS::Cell);
    GC_DECLARE_ALLOCATOR(NetworkBodyPump);

public:
    virtual ~NetworkBodyPump() override;

    void set_response(GC::Ref<Fetch::Infrastructure::Response const> response) { m_response = response; }
    void set_body(GC::Ref<Fetch::Infrastructure::Body> body);

    void handle_network_data(Requests::ResponseData);
    void handle_network_complete();
    void handle_network_error(StringView error_message);
    void set_cached_response_body(Core::ImmutableBytes);

    // Drops the response's pending memory-cache entry, for a body that will not finish arriving.
    void abandon_cache_entry();

private:
    NetworkBodyPump(GC::Ptr<Infrastructure::FetchParams const>, NonnullRefPtr<Engine::FetchByteChannel>, NonnullRefPtr<Engine::FetchBodyDeliveryGate>, RefPtr<HTTP::MemoryCache>);

    virtual void visit_edges(Visitor& visitor) override;

    void finalize_cache_entry();

    GC::Ptr<Infrastructure::FetchParams const> m_fetch_params;
    GC::Ptr<Fetch::Infrastructure::Response const> m_response;
    GC::Ptr<Fetch::Infrastructure::Body> m_body;

    NonnullRefPtr<Engine::FetchByteChannel> m_channel;
    NonnullRefPtr<Engine::FetchBodyDeliveryGate> m_delivery_gate;

    RefPtr<HTTP::MemoryCache> m_http_cache;

    // Bytes received before set_body(), flushed into the body's sniff buffer once it is attached.
    ByteBuffer m_pre_body_sniff_buffer;

    // Whole-response buffer retained only when m_http_cache is non-null, for finalize_entry().
    ByteBuffer m_cache_buffer;
    Optional<Core::ImmutableBytes> m_cache_body;
    bool m_cache_body_replaces_network_buffer { false };

    bool m_network_complete { false };
    bool m_network_failed { false };
};

}
