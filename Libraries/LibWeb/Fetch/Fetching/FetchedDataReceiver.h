/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibGC/CellAllocator.h>
#include <LibHTTP/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

class FetchedDataReceiver final : public JS::Cell {
    GC_CELL(FetchedDataReceiver, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchedDataReceiver);

public:
    virtual ~FetchedDataReceiver() override;

    void set_response(GC::Ref<Fetch::Infrastructure::Response const> response) { m_response = response; }
    void set_body(GC::Ref<Fetch::Infrastructure::Body> body);

    enum class NetworkState {
        Ongoing,
        Complete,
        Error,
    };
    void handle_network_bytes(ReadonlyBytes, NetworkState);

private:
    FetchedDataReceiver(GC::Ref<Infrastructure::FetchParams const>, GC::Ref<Streams::ReadableStream>, RefPtr<HTTP::MemoryCache>);

    virtual void visit_edges(Visitor& visitor) override;

    void enqueue_into_stream(ReadonlyBytes);
    void close_stream();

    GC::Ref<Infrastructure::FetchParams const> m_fetch_params;
    GC::Ptr<Fetch::Infrastructure::Response const> m_response;
    GC::Ptr<Fetch::Infrastructure::Body> m_body;

    GC::Ref<Streams::ReadableStream> m_stream;

    RefPtr<HTTP::MemoryCache> m_http_cache;

    // Bytes received before set_body() is called. Held only until the body is attached and these
    // are flushed into the body's MIME-sniff buffer.
    ByteBuffer m_pre_body_sniff_buffer;

    // Whole-response buffer retained only when m_http_cache is non-null, for finalize_entry().
    ByteBuffer m_cache_buffer;

    bool m_network_complete { false };
};

}
