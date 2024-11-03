/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Fetch/Infrastructure/IncrementalReadLoopReadRequest.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::Fetch::Infrastructure {

JS_DEFINE_ALLOCATOR(IncrementalReadLoopReadRequest);

void IncrementalReadLoopReadRequest::on_chunk(JS::Value chunk)
{
    auto& realm = m_reader->realm();
    // 1. Let continueAlgorithm be null.
    JS::GCPtr<JS::HeapFunction<void()>> continue_algorithm;

    // 2. If chunk is not a Uint8Array object, then set continueAlgorithm to this step: run processBodyError given a TypeError.
    if (!chunk.is_object() || !is<JS::Uint8Array>(chunk.as_object())) {
        continue_algorithm = JS::create_heap_function(realm.heap(), [&realm, process_body_error = m_process_body_error] {
            process_body_error->function()(JS::TypeError::create(realm, "Chunk data is not Uint8Array"sv));
        });
    }
    // 3. Otherwise:
    else {
        // 1. Let bytes be a copy of chunk.
        // NOTE: Implementations are strongly encouraged to use an implementation strategy that avoids this copy where possible.
        auto& uint8_array = static_cast<JS::Uint8Array&>(chunk.as_object());
        auto bytes = MUST(ByteBuffer::copy(uint8_array.data()));
        // 2. Set continueAlgorithm to these steps:
        continue_algorithm = JS::create_heap_function(realm.heap(), [bytes = move(bytes), body = m_body, reader = m_reader, task_destination = m_task_destination, process_body_chunk = m_process_body_chunk, process_end_of_body = m_process_end_of_body, process_body_error = m_process_body_error] {
            HTML::TemporaryExecutionContext execution_context { reader->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            // 1. Run processBodyChunk given bytes.
            process_body_chunk->function()(move(bytes));

            // 2. Perform the incrementally-read loop given reader, taskDestination, processBodyChunk, processEndOfBody, and processBodyError.
            body->incrementally_read_loop(reader, task_destination, process_body_chunk, process_end_of_body, process_body_error);
        });
    }

    // 4. Queue a fetch task given continueAlgorithm and taskDestination.
    Fetch::Infrastructure::queue_fetch_task(m_task_destination, *continue_algorithm);
}

void IncrementalReadLoopReadRequest::on_close()
{
    // 1. Queue a fetch task given processEndOfBody and taskDestination.
    Fetch::Infrastructure::queue_fetch_task(m_task_destination, JS::create_heap_function(m_reader->heap(), [this] {
        m_process_end_of_body->function()();
    }));
}

void IncrementalReadLoopReadRequest::on_error(JS::Value error)
{
    // 1. Queue a fetch task to run processBodyError given e, with taskDestination.
    Fetch::Infrastructure::queue_fetch_task(m_task_destination, JS::create_heap_function(m_reader->heap(), [this, error = move(error)] {
        m_process_body_error->function()(error);
    }));
}

IncrementalReadLoopReadRequest::IncrementalReadLoopReadRequest(JS::NonnullGCPtr<Body> body, JS::NonnullGCPtr<Streams::ReadableStreamDefaultReader> reader, JS::NonnullGCPtr<JS::Object> task_destination, Body::ProcessBodyChunkCallback process_body_chunk, Body::ProcessEndOfBodyCallback process_end_of_body, Body::ProcessBodyErrorCallback process_body_error)
    : m_body(body)
    , m_reader(reader)
    , m_task_destination(task_destination)
    , m_process_body_chunk(process_body_chunk)
    , m_process_end_of_body(process_end_of_body)
    , m_process_body_error(process_body_error)
{
}

void IncrementalReadLoopReadRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_body);
    visitor.visit(m_reader);
    visitor.visit(m_task_destination);
    visitor.visit(m_process_body_chunk);
    visitor.visit(m_process_end_of_body);
    visitor.visit(m_process_body_error);
}

}
