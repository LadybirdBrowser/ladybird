/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

class FetchedDataReceiver final : public JS::Cell {
    JS_CELL(FetchedDataReceiver, JS::Cell);
    JS_DECLARE_ALLOCATOR(FetchedDataReceiver);

public:
    virtual ~FetchedDataReceiver() override;

    void set_pending_promise(JS::NonnullGCPtr<WebIDL::Promise>);
    void on_data_received(ReadonlyBytes);

private:
    FetchedDataReceiver(JS::NonnullGCPtr<Infrastructure::FetchParams const>, JS::NonnullGCPtr<Streams::ReadableStream>);

    virtual void visit_edges(Visitor& visitor) override;

    JS::NonnullGCPtr<Infrastructure::FetchParams const> m_fetch_params;
    JS::NonnullGCPtr<Streams::ReadableStream> m_stream;
    JS::GCPtr<WebIDL::Promise> m_pending_promise;
    ByteBuffer m_buffer;
};

}
