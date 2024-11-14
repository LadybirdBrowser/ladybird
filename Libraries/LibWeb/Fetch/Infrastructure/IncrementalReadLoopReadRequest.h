/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#incrementally-read-loop
class IncrementalReadLoopReadRequest : public Streams::ReadRequest {
    GC_CELL(IncrementalReadLoopReadRequest, Streams::ReadRequest);
    GC_DECLARE_ALLOCATOR(IncrementalReadLoopReadRequest);

public:
    IncrementalReadLoopReadRequest(GC::Ref<Body>, GC::Ref<Streams::ReadableStreamDefaultReader>, GC::Ref<JS::Object> task_destination, Body::ProcessBodyChunkCallback, Body::ProcessEndOfBodyCallback, Body::ProcessBodyErrorCallback);

    virtual void on_chunk(JS::Value chunk) override;
    virtual void on_close() override;
    virtual void on_error(JS::Value error) override;

private:
    virtual void visit_edges(Visitor&) override;

    GC::Ref<Body> m_body;
    GC::Ref<Streams::ReadableStreamDefaultReader> m_reader;
    GC::Ref<JS::Object> m_task_destination;
    Body::ProcessBodyChunkCallback m_process_body_chunk;
    Body::ProcessEndOfBodyCallback m_process_end_of_body;
    Body::ProcessBodyErrorCallback m_process_body_error;
};

}
