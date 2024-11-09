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
    JS_CELL(IncrementalReadLoopReadRequest, Streams::ReadRequest);
    JS_DECLARE_ALLOCATOR(IncrementalReadLoopReadRequest);

public:
    IncrementalReadLoopReadRequest(JS::NonnullGCPtr<Body>, JS::NonnullGCPtr<Streams::ReadableStreamDefaultReader>, JS::NonnullGCPtr<JS::Object> task_destination, Body::ProcessBodyChunkCallback, Body::ProcessEndOfBodyCallback, Body::ProcessBodyErrorCallback);

    virtual void on_chunk(JS::Value chunk) override;
    virtual void on_close() override;
    virtual void on_error(JS::Value error) override;

private:
    virtual void visit_edges(Visitor&) override;

    JS::NonnullGCPtr<Body> m_body;
    JS::NonnullGCPtr<Streams::ReadableStreamDefaultReader> m_reader;
    JS::NonnullGCPtr<JS::Object> m_task_destination;
    Body::ProcessBodyChunkCallback m_process_body_chunk;
    Body::ProcessEndOfBodyCallback m_process_end_of_body;
    Body::ProcessBodyErrorCallback m_process_body_error;
};

}
