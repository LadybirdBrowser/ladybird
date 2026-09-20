/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/IncrementalReadLoopReadRequest.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(Body);

FetchBody::FetchBody(Optional<Core::ImmutableBytes> source, Optional<u64> length)
    : m_source(move(source))
    , m_length(length)
{
}

void FetchBody::set_source(Core::ImmutableBytes source, Optional<u64> length)
{
    m_source = move(source);
    m_length = length;
}

void FetchBody::append_sniff_bytes(ReadonlyBytes bytes)
{
    if (m_sniff_outcome.has_value())
        return;

    size_t space_remaining = MAX_SNIFF_BYTES - m_sniff_bytes.size();
    if (space_remaining == 0) {
        set_sniff_bytes_complete();
        return;
    }

    size_t to_append = min(bytes.size(), space_remaining);
    m_sniff_bytes.append(bytes.slice(0, to_append));

    if (m_sniff_bytes.size() >= MAX_SNIFF_BYTES)
        set_sniff_bytes_complete();
}

void FetchBody::set_sniff_bytes_complete()
{
    if (!m_sniff_outcome.has_value())
        m_sniff_outcome = SniffOutcome::Complete;
}

void FetchBody::set_sniff_bytes_failed()
{
    if (!m_sniff_outcome.has_value())
        m_sniff_outcome = SniffOutcome::Failed;
}

Optional<FetchBody::SniffBytes> FetchBody::sniff_bytes_if_available() const
{
    // Non-streaming body: source has bytes
    if (m_source.has_value()) {
        auto bytes = m_source->bytes();
        return SniffBytes { bytes.slice(0, min(bytes.size(), MAX_SNIFF_BYTES)) };
    }

    // Streaming body: bytes captured during fetch
    if (m_sniff_outcome.has_value())
        return SniffBytes { m_sniff_bytes, *m_sniff_outcome };

    // Still waiting for bytes
    return {};
}

GC::Ref<Body> Body::create(GC::Ref<Streams::ReadableStream> stream, FetchBody fetch_body)
{
    return GC::Heap::the().allocate<Body>(stream, move(fetch_body));
}

Body::Body(GC::Ref<Streams::ReadableStream> stream, FetchBody fetch_body)
    : m_stream(stream)
    , m_fetch_body(move(fetch_body))
{
}

void Body::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stream);
    visitor.visit(m_sniff_bytes_callback);
}

void Body::set_source(Core::ImmutableBytes source, Optional<u64> length)
{
    m_fetch_body.set_source(move(source), length);
    notify_sniff_bytes_waiter();
}

void Body::append_sniff_bytes(ReadonlyBytes bytes)
{
    m_fetch_body.append_sniff_bytes(bytes);
    notify_sniff_bytes_waiter();
}

void Body::set_sniff_bytes_complete()
{
    m_fetch_body.set_sniff_bytes_complete();
    notify_sniff_bytes_waiter();
}

void Body::set_sniff_bytes_failed()
{
    m_fetch_body.set_sniff_bytes_failed();
    notify_sniff_bytes_waiter();
}

void Body::wait_for_sniff_bytes(SniffBytesCallback on_ready)
{
    if (auto sniff_bytes = sniff_bytes_if_available(); sniff_bytes.has_value()) {
        on_ready->function()(sniff_bytes.value());
        return;
    }

    // Wait for bytes to arrive
    m_sniff_bytes_callback = on_ready;
}

void Body::notify_sniff_bytes_waiter()
{
    if (!m_sniff_bytes_callback)
        return;
    auto sniff_bytes = sniff_bytes_if_available();
    if (!sniff_bytes.has_value())
        return;
    auto callback = exchange(m_sniff_bytes_callback, nullptr);
    callback->function()(sniff_bytes.value());
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
    return Body::create(*out2, m_fetch_body);
}

// https://fetch.spec.whatwg.org/#body-fully-read
void Body::fully_read(JS::Realm& realm, Web::Fetch::Infrastructure::Body::ProcessBodyCallback process_body, Web::Fetch::Infrastructure::Body::ProcessBodyErrorCallback process_body_error, TaskDestination task_destination) const
{
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. If taskDestination is null, then set taskDestination to the result of starting a new parallel queue.
    if (task_destination.has<Empty>())
        task_destination = HTML::ParallelQueue::create();

    // 2. Let successSteps given a byte sequence bytes be to queue a fetch task to run processBody given bytes, with taskDestination.
    auto success_steps = [process_body, task_destination](ByteBuffer bytes) {
        queue_fetch_task(task_destination, GC::create_function(GC::Heap::the(), [process_body, bytes = move(bytes)]() mutable {
            process_body->function()(move(bytes));
        }));
    };

    // 3. Let errorSteps optionally given an exception exception be to queue a fetch task to run processBodyError given
    //    exception, with taskDestination.
    auto error_steps = [process_body_error, task_destination](JS::Value exception) {
        queue_fetch_task(task_destination, GC::create_function(GC::Heap::the(), [process_body_error, exception]() {
            process_body_error->function()(exception);
        }));
    };

    // 4. Let reader be the result of getting a reader for body’s stream. If that threw an exception, then run errorSteps
    //    with that exception and return.
    auto reader = m_stream->get_a_reader();

    if (reader.is_exception()) {
        auto throw_completion = WebIDL::exception_to_throw_completion(realm.vm(), realm, reader.release_error());
        error_steps(throw_completion.release_value());
        return;
    }

    // 5. Read all bytes from reader, given successSteps and errorSteps.
    reader.value()->read_all_bytes(GC::create_function(GC::Heap::the(), move(success_steps)), GC::create_function(GC::Heap::the(), move(error_steps)));
}

// https://fetch.spec.whatwg.org/#body-incrementally-read
GC::Ref<Streams::ReadableStreamDefaultReader> Body::incrementally_read(JS::Realm& realm, ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination)
{
    HTML::TemporaryExecutionContext const execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. If taskDestination is null, then set taskDestination to the result of starting a new parallel queue.
    if (task_destination.has<Empty>())
        task_destination = HTML::ParallelQueue::create();

    // 2. Let reader be the result of getting a reader for body’s stream.
    // NOTE: This operation will not throw an exception.
    auto reader = MUST(m_stream->get_a_reader());

    // 3. Perform the incrementally-read loop given reader, taskDestination, processBodyChunk, processEndOfBody, and processBodyError.
    VERIFY(!task_destination.has<Empty>());
    incrementally_read_loop(reader, task_destination, process_body_chunk, process_end_of_body, process_body_error);
    return reader;
}

GC::Ref<Streams::ReadableStreamDefaultReader> Body::incrementally_read(ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error, TaskDestination task_destination)
{
    VERIFY(task_destination.has<GC::Ref<JS::Object>>());
    return incrementally_read(HTML::relevant_realm(*task_destination.get<GC::Ref<JS::Object>>()), move(process_body_chunk), move(process_end_of_body), move(process_body_error), move(task_destination));
}

// https://fetch.spec.whatwg.org/#incrementally-read-loop
void Body::incrementally_read_loop(Streams::ReadableStreamDefaultReader& reader, TaskDestination task_destination, ProcessBodyChunkCallback process_body_chunk, ProcessEndOfBodyCallback process_end_of_body, ProcessBodyErrorCallback process_body_error)
{
    // 1. Let readRequest be the following read request:
    auto read_request = GC::Heap::the().allocate<IncrementalReadLoopReadRequest>(*this, reader, task_destination, process_body_chunk, process_end_of_body, process_body_error);

    // 2. Read a chunk from reader given readRequest.
    reader.read_a_chunk(read_request);
}

// https://fetch.spec.whatwg.org/#byte-sequence-as-a-body
GC::Ref<Body> byte_sequence_as_body(JS::Realm& realm, ReadonlyBytes bytes)
{
    // To get a byte sequence bytes as a body, return the body of the result of safely extracting bytes.
    auto [body, _] = safely_extract_body(realm, bytes);
    return body;
}

void cancel_incremental_read(Streams::ReadableStreamDefaultReader& reader)
{
    reader.cancel({});
}

}
