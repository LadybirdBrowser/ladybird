/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/IncrementalReadLoopReadRequest.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(Body);

GC::Ref<Body> Body::create(JS::VM& vm, GC::Ref<Streams::ReadableStream> stream)
{
    return vm.heap().allocate<Body>(stream);
}

GC::Ref<Body> Body::create(JS::VM& vm, GC::Ref<Streams::ReadableStream> stream, SourceType source, Optional<u64> length)
{
    return vm.heap().allocate<Body>(stream, source, length);
}

Body::Body(GC::Ref<Streams::ReadableStream> stream)
    : m_stream(move(stream))
{
}

Body::Body(GC::Ref<Streams::ReadableStream> stream, SourceType source, Optional<u64> length)
    : m_stream(move(stream))
    , m_source(move(source))
    , m_length(move(length))
{
}

void Body::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stream);
}

// https://fetch.spec.whatwg.org/#concept-body-clone
GC::Ref<Body> Body::clone(JS::Realm& realm)
{
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // To clone a body body, run these steps:
    // 1. Let « out1, out2 » be the result of teeing body’s stream.
    auto [out1, out2] = m_stream->tee().release_value_but_fixme_should_propagate_errors();

    // 2. Set body’s stream to out1.
    m_stream = out1;

    // 3. Return a body whose stream is out2 and other members are copied from body.
    return Body::create(realm.vm(), *out2, m_source, m_length);
}

// https://fetch.spec.whatwg.org/#body-fully-read
void Body::fully_read(JS::Realm& realm, ProcessBodyCallback process_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination) const
{
    // FIXME: 1. If taskDestination is null, then set taskDestination to the result of starting a new parallel queue.
    // FIXME: Handle 'parallel queue' task destination
    VERIFY(!task_destination.has<Empty>());
    auto task_destination_object = task_destination.get<GC::Ref<JS::Object>>();

    // 2. Let successSteps given a byte sequence bytes be to queue a fetch task to run processBody given bytes, with taskDestination.
    auto success_steps = create_function(realm.heap(), [&realm, process_body, task_destination_object = task_destination_object](ByteBuffer bytes) -> void {
        // Make a copy of the bytes, as the source of the bytes may disappear between the time the task is queued and executed.
        auto bytes_copy = MUST(ByteBuffer::copy(bytes));
        queue_fetch_task(*task_destination_object, GC::create_function(realm.heap(), [process_body, bytes_copy = move(bytes_copy)]() mutable {
            process_body->function()(move(bytes_copy));
        }));
    });

    // 3. Let errorSteps optionally given an exception exception be to queue a fetch task to run processBodyError given exception, with taskDestination.
    auto error_steps = create_function(realm.heap(), [&realm, process_body_error, task_destination_object](JS::Value exception) {
        queue_fetch_task(*task_destination_object, GC::create_function(realm.heap(), [process_body_error, exception]() {
            process_body_error->function()(exception);
        }));
    });

    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 4. Let reader be the result of getting a reader for body’s stream. If that threw an exception, then run errorSteps with that exception and return.
    auto reader_or_exception = acquire_readable_stream_default_reader(m_stream);
    if (reader_or_exception.is_exception()) {
        auto exception = reader_or_exception.release_error();
        // FIXME: Is that safe to assume that the exception is a DOMException?
        auto dom_exception = exception.get<GC::Ref<WebIDL::DOMException>>();
        error_steps->function()(dom_exception);
        return;
    }

    // 5. Read all bytes from reader, given successSteps and errorSteps.
    reader_or_exception.release_value()->read_all_bytes(success_steps, error_steps);
}

// https://fetch.spec.whatwg.org/#body-incrementally-read
void Body::incrementally_read(ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination)
{
    HTML::TemporaryExecutionContext const execution_context { m_stream->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    VERIFY(task_destination.has<GC::Ref<JS::Object>>());
    // FIXME: 1. If taskDestination is null, then set taskDestination to the result of starting a new parallel queue.
    // FIXME: Handle 'parallel queue' task destination

    // 2. Let reader be the result of getting a reader for body’s stream.
    // NOTE: This operation will not throw an exception.
    auto reader = MUST(Streams::acquire_readable_stream_default_reader(m_stream));

    // 3. Perform the incrementally-read loop given reader, taskDestination, processBodyChunk, processEndOfBody, and processBodyError.
    incrementally_read_loop(reader, task_destination.get<GC::Ref<JS::Object>>(), process_body_chunk, process_end_of_body, process_body_error);
}

// https://fetch.spec.whatwg.org/#incrementally-read-loop
void Body::incrementally_read_loop(Streams::ReadableStreamDefaultReader& reader, GC::Ref<JS::Object> task_destination, ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error)

{
    auto& realm = reader.realm();
    // 1. Let readRequest be the following read request:
    auto read_request = realm.create<IncrementalReadLoopReadRequest>(*this, reader, task_destination, process_body_chunk, process_end_of_body, process_body_error);

    // 2. Read a chunk from reader given readRequest.
    reader.read_a_chunk(read_request);
}

// https://fetch.spec.whatwg.org/#byte-sequence-as-a-body
WebIDL::ExceptionOr<GC::Ref<Body>> byte_sequence_as_body(JS::Realm& realm, ReadonlyBytes bytes)
{
    // To get a byte sequence bytes as a body, return the body of the result of safely extracting bytes.
    auto [body, _] = TRY(safely_extract_body(realm, bytes));
    return body;
}

}
