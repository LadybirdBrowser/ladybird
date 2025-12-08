/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/ReadableStreamPipeTo.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultWriter.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams::Detail {

class ReadableStreamPipeToReadRequest final : public ReadRequest {
    GC_CELL(ReadableStreamPipeToReadRequest, ReadRequest);
    GC_DECLARE_ALLOCATOR(ReadableStreamPipeToReadRequest);

    using OnChunk = GC::Ref<GC::Function<void(JS::Value)>>;
    using OnComplete = GC::Ref<GC::Function<void()>>;

    // This has a return value just for compatibility with WebIDL::react_to_promise.
    using OnError = GC::Ref<GC::Function<WebIDL::ExceptionOr<JS::Value>(JS::Value)>>;

public:
    virtual void on_chunk(JS::Value chunk) override
    {
        m_on_chunk->function()(chunk);
    }

    virtual void on_close() override
    {
        m_on_complete->function()();
    }

    virtual void on_error(JS::Value error) override
    {
        MUST(m_on_error->function()(error));
    }

private:
    ReadableStreamPipeToReadRequest(OnChunk on_chunk, OnComplete on_complete, OnError on_error)
        : m_on_chunk(on_chunk)
        , m_on_complete(on_complete)
        , m_on_error(on_error)
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_on_chunk);
        visitor.visit(m_on_complete);
        visitor.visit(m_on_error);
    }

    OnChunk m_on_chunk;
    OnComplete m_on_complete;
    OnError m_on_error;
};

GC_DEFINE_ALLOCATOR(ReadableStreamPipeTo);
GC_DEFINE_ALLOCATOR(ReadableStreamPipeToReadRequest);

// https://streams.spec.whatwg.org/#ref-for-in-parallel
ReadableStreamPipeTo::ReadableStreamPipeTo(
    GC::Ref<JS::Realm> realm,
    GC::Ref<WebIDL::Promise> promise,
    GC::Ref<ReadableStream> source,
    GC::Ref<WritableStream> destination,
    GC::Ref<ReadableStreamDefaultReader> reader,
    GC::Ref<WritableStreamDefaultWriter> writer,
    bool prevent_close,
    bool prevent_abort,
    bool prevent_cancel)
    : m_realm(realm)
    , m_promise(promise)
    , m_source(source)
    , m_destination(destination)
    , m_reader(reader)
    , m_writer(writer)
    , m_on_shutdown(GC::create_function(heap(), [this](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        check_for_error_and_close_states();
        return JS::js_undefined();
    }))
    , m_prevent_close(prevent_close)
    , m_prevent_abort(prevent_abort)
    , m_prevent_cancel(prevent_cancel)
{
    m_reader->set_readable_stream_pipe_to_operation({}, this);

    if (auto reader_closed_promise = m_reader->closed())
        WebIDL::react_to_promise(*reader_closed_promise, m_on_shutdown, m_on_shutdown);
    if (auto writer_closed_promise = m_writer->closed())
        WebIDL::react_to_promise(*writer_closed_promise, m_on_shutdown, m_on_shutdown);
}

void ReadableStreamPipeTo::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_promise);
    visitor.visit(m_source);
    visitor.visit(m_destination);
    visitor.visit(m_reader);
    visitor.visit(m_writer);
    visitor.visit(m_signal);
    visitor.visit(m_last_write_promise);
    visitor.visit(m_unwritten_chunks);
    visitor.visit(m_on_shutdown);
}

void ReadableStreamPipeTo::process()
{
    if (check_for_error_and_close_states())
        return;

    auto ready_promise = m_writer->ready();

    if (ready_promise && WebIDL::is_promise_fulfilled(*ready_promise)) {
        read_chunk();
        return;
    }

    auto when_ready = GC::create_function(m_realm->heap(), [this](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        read_chunk();
        return JS::js_undefined();
    });

    if (ready_promise)
        WebIDL::react_to_promise(*ready_promise, when_ready, m_on_shutdown);
}

void ReadableStreamPipeTo::set_abort_signal(GC::Ref<DOM::AbortSignal> signal, DOM::AbortSignal::AbortSignal::AbortAlgorithmID signal_id)
{
    m_signal = signal;
    m_signal_id = signal_id;
}

// https://streams.spec.whatwg.org/#rs-pipeTo-shutdown-with-action
void ReadableStreamPipeTo::shutdown_with_action(GC::Ref<GC::Function<GC::Ref<WebIDL::Promise>()>> action, Optional<JS::Value> original_error)
{
    // 1. If shuttingDown is true, abort these substeps.
    if (m_shutting_down)
        return;

    // 2. Set shuttingDown to true.
    m_shutting_down = true;

    auto on_pending_writes_complete = [this, action, original_error = move(original_error)]() mutable {
        HTML::TemporaryExecutionContext execution_context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 4. Let p be the result of performing action.
        auto promise = action->function()();

        WebIDL::react_to_promise(promise,
            // 5. Upon fulfillment of p, finalize, passing along originalError if it was given.
            GC::create_function(heap(), [this, original_error = move(original_error)](JS::Value) mutable -> WebIDL::ExceptionOr<JS::Value> {
                finish(move(original_error));
                return JS::js_undefined();
            }),

            // 6. Upon rejection of p with reason newError, finalize with newError.
            GC::create_function(heap(), [this](JS::Value new_error) -> WebIDL::ExceptionOr<JS::Value> {
                finish(new_error);
                return JS::js_undefined();
            }));
    };

    // 3. If dest.[[state]] is "writable" and ! WritableStreamCloseQueuedOrInFlight(dest) is false,
    if (m_destination->state() == WritableStream::State::Writable && !writable_stream_close_queued_or_in_flight(m_destination)) {
        // 1. If any chunks have been read but not yet written, write them to dest.
        write_unwritten_chunks();

        // 2. Wait until every chunk that has been read has been written (i.e. the corresponding promises have settled).
        wait_for_pending_writes_to_complete(move(on_pending_writes_complete));
    } else {
        on_pending_writes_complete();
    }
}

// https://streams.spec.whatwg.org/#rs-pipeTo-shutdown
void ReadableStreamPipeTo::shutdown(Optional<JS::Value> error)
{
    // 1. If shuttingDown is true, abort these substeps.
    if (m_shutting_down)
        return;

    // 2. Set shuttingDown to true.
    m_shutting_down = true;

    auto on_pending_writes_complete = [this, error = move(error)]() mutable {
        HTML::TemporaryExecutionContext execution_context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 4. Finalize, passing along error if it was given.
        finish(move(error));
    };

    // 3. If dest.[[state]] is "writable" and ! WritableStreamCloseQueuedOrInFlight(dest) is false,
    if (m_destination->state() == WritableStream::State::Writable && !writable_stream_close_queued_or_in_flight(m_destination)) {
        // 1. If any chunks have been read but not yet written, write them to dest.
        write_unwritten_chunks();

        // 2. Wait until every chunk that has been read has been written (i.e. the corresponding promises have settled).
        wait_for_pending_writes_to_complete(move(on_pending_writes_complete));
    } else {
        on_pending_writes_complete();
    }
}

void ReadableStreamPipeTo::read_chunk()
{
    // Shutdown must stop activity: if shuttingDown becomes true, the user agent must not initiate further reads from
    // reader, and must only perform writes of already-read chunks, as described below. In particular, the user agent
    // must check the below conditions before performing any reads or writes, since they might lead to immediate shutdown.
    if (check_for_error_and_close_states())
        return;

    auto on_chunk = GC::create_function(heap(), [this](JS::Value chunk) {
        m_unwritten_chunks.append(chunk);

        if (check_for_error_and_close_states())
            return;

        HTML::queue_a_microtask(nullptr, GC::create_function(m_realm->heap(), [this]() {
            HTML::TemporaryExecutionContext execution_context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            write_chunk();
            process();
        }));
    });

    auto on_complete = GC::create_function(heap(), [this]() {
        if (!check_for_error_and_close_states())
            finish();
    });

    auto read_request = heap().allocate<ReadableStreamPipeToReadRequest>(on_chunk, on_complete, *m_on_shutdown);
    readable_stream_default_reader_read(m_reader, read_request);
}

void ReadableStreamPipeTo::write_chunk()
{
    // Shutdown must stop activity: if shuttingDown becomes true, the user agent must not initiate further reads from
    // reader, and must only perform writes of already-read chunks, as described below. In particular, the user agent
    // must check the below conditions before performing any reads or writes, since they might lead to immediate shutdown.
    if (!m_shutting_down && check_for_error_and_close_states())
        return;

    auto promise = writable_stream_default_writer_write(m_writer, m_unwritten_chunks.take_first());
    WebIDL::mark_promise_as_handled(promise);

    m_last_write_promise = promise;
}

void ReadableStreamPipeTo::write_unwritten_chunks()
{
    while (!m_unwritten_chunks.is_empty())
        write_chunk();
}

void ReadableStreamPipeTo::wait_for_pending_writes_to_complete(Function<void()> on_complete)
{
    auto last_write_promise = m_last_write_promise;
    m_last_write_promise = {};
    if (!last_write_promise) {
        HTML::queue_a_microtask(nullptr, GC::create_function(heap(), [on_complete = move(on_complete)]() {
            on_complete();
        }));
        return;
    }
    auto run_complete_steps = GC::create_function(heap(), [on_complete = move(on_complete)](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        on_complete();
        return JS::js_undefined();
    });
    WebIDL::react_to_promise(*last_write_promise, run_complete_steps, run_complete_steps);
}

// https://streams.spec.whatwg.org/#rs-pipeTo-finalize
// We call this `finish` instead of `finalize` to avoid conflicts with GC::Cell::finalize.
void ReadableStreamPipeTo::finish(Optional<JS::Value> error)
{
    // 1. Perform ! WritableStreamDefaultWriterRelease(writer).
    writable_stream_default_writer_release(m_writer);

    // 2. If reader implements ReadableStreamBYOBReader, perform ! ReadableStreamBYOBReaderRelease(reader).
    // 3. Otherwise, perform ! ReadableStreamDefaultReaderRelease(reader).
    readable_stream_default_reader_release(m_reader);

    // 4. If signal is not undefined, remove abortAlgorithm from signal.
    if (m_signal)
        m_signal->remove_abort_algorithm(m_signal_id);

    // 5. If error was given, reject promise with error.
    if (error.has_value()) {
        WebIDL::reject_promise(m_realm, m_promise, *error);
    }
    // 6. Otherwise, resolve promise with undefined.
    else {
        WebIDL::resolve_promise(m_realm, m_promise, JS::js_undefined());
    }

    m_reader->set_readable_stream_pipe_to_operation({}, nullptr);
}

bool ReadableStreamPipeTo::check_for_error_and_close_states()
{
    // Error and close states must be propagated: the following conditions must be applied in order.
    return m_shutting_down
        || check_for_forward_errors()
        || check_for_backward_errors()
        || check_for_forward_close()
        || check_for_backward_close();
}

bool ReadableStreamPipeTo::check_for_forward_errors()
{
    // 1. Errors must be propagated forward: if source.[[state]] is or becomes "errored", then
    if (m_source->state() == ReadableStream::State::Errored) {
        // 1. If preventAbort is false, shutdown with an action of ! WritableStreamAbort(dest, source.[[storedError]])
        //    and with source.[[storedError]].
        if (!m_prevent_abort) {
            auto action = GC::create_function(heap(), [this]() {
                return writable_stream_abort(m_destination, m_source->stored_error());
            });

            shutdown_with_action(action, m_source->stored_error());
        }
        // 2. Otherwise, shutdown with source.[[storedError]].
        else {
            shutdown(m_source->stored_error());
        }
    }

    return m_shutting_down;
}

bool ReadableStreamPipeTo::check_for_backward_errors()
{
    // 2. Errors must be propagated backward: if dest.[[state]] is or becomes "errored", then
    if (m_destination->state() == WritableStream::State::Errored) {
        // 1. If preventCancel is false, shutdown with an action of ! ReadableStreamCancel(source, dest.[[storedError]])
        //    and with dest.[[storedError]].
        if (!m_prevent_cancel) {
            auto action = GC::create_function(heap(), [this]() {
                return readable_stream_cancel(m_source, m_destination->stored_error());
            });

            shutdown_with_action(action, m_destination->stored_error());
        }
        // 2. Otherwise, shutdown with dest.[[storedError]].
        else {
            shutdown(m_destination->stored_error());
        }
    }

    return m_shutting_down;
}

bool ReadableStreamPipeTo::check_for_forward_close()
{
    // 3. Closing must be propagated forward: if source.[[state]] is or becomes "closed", then
    if (m_source->state() == ReadableStream::State::Closed) {
        // 1. If preventClose is false, shutdown with an action of ! WritableStreamDefaultWriterCloseWithErrorPropagation(writer).
        if (!m_prevent_close) {
            auto action = GC::create_function(heap(), [this]() {
                return writable_stream_default_writer_close_with_error_propagation(m_writer);
            });

            shutdown_with_action(action);
        }
        // 2. Otherwise, shutdown.
        else {
            shutdown();
        }
    }

    return m_shutting_down;
}

bool ReadableStreamPipeTo::check_for_backward_close()
{
    // 4. Closing must be propagated backward: if ! WritableStreamCloseQueuedOrInFlight(dest) is true or dest.[[state]] is "closed", then
    if (writable_stream_close_queued_or_in_flight(m_destination) || m_destination->state() == WritableStream::State::Closed) {
        // 1. Assert: no chunks have been read or written.

        // 2. Let destClosed be a new TypeError.
        auto destination_closed = JS::TypeError::create(m_realm, "Destination stream was closed during piping operation"sv);

        // 3. If preventCancel is false, shutdown with an action of ! ReadableStreamCancel(source, destClosed) and with destClosed.
        if (!m_prevent_cancel) {
            auto action = GC::create_function(heap(), [this, destination_closed]() {
                return readable_stream_cancel(m_source, destination_closed);
            });

            shutdown_with_action(action, destination_closed);
        }
        // 4. Otherwise, shutdown with destClosed.
        else {
            shutdown(destination_closed);
        }
    }

    return m_shutting_down;
}

}
