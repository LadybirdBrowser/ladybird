/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

class FetchedDataReceiver final : public JS::Cell {
    GC_CELL(FetchedDataReceiver, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchedDataReceiver);

public:
    virtual ~FetchedDataReceiver() override;

    void set_pending_promise(GC::Ref<WebIDL::Promise>);

    enum class NetworkState {
        Ongoing,
        Complete,
        Error,
    };
    void handle_network_bytes(ReadonlyBytes, NetworkState);

private:
    FetchedDataReceiver(GC::Ref<Infrastructure::FetchParams const>, GC::Ref<Streams::ReadableStream>);

    virtual void visit_edges(Visitor& visitor) override;

    void pull_bytes_into_stream(ByteBuffer&&);
    void close_stream();

    GC::Ref<Infrastructure::FetchParams const> m_fetch_params;
    GC::Ref<Streams::ReadableStream> m_stream;
    GC::Ptr<WebIDL::Promise> m_pending_promise;

    ByteBuffer m_buffer;
    enum class LifecycleState {
        Receiving,
        CompletePending,
        ReadyToClose,
        Closed,
    };
    LifecycleState m_lifecycle_state { LifecycleState::Receiving };
    bool m_has_unfulfilled_promise { false };
};

}
