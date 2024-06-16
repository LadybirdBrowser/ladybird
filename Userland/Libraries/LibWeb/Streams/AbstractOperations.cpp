/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/DataViewConstructor.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamBYOBReader.h>
#include <LibWeb/Streams/ReadableStreamBYOBRequest.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamGenericReader.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>
#include <LibWeb/Streams/Transformer.h>
#include <LibWeb/Streams/UnderlyingSink.h>
#include <LibWeb/Streams/UnderlyingSource.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultController.h>
#include <LibWeb/Streams/WritableStreamDefaultWriter.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#acquire-readable-stream-reader
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStreamDefaultReader>> acquire_readable_stream_default_reader(ReadableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let reader be a new ReadableStreamDefaultReader.
    auto reader = realm.heap().allocate<ReadableStreamDefaultReader>(realm, realm);

    // 2. Perform ? SetUpReadableStreamDefaultReader(reader, stream).
    TRY(set_up_readable_stream_default_reader(reader, stream));

    // 3. Return reader.
    return reader;
}

// https://streams.spec.whatwg.org/#acquire-readable-stream-byob-reader
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStreamBYOBReader>> acquire_readable_stream_byob_reader(ReadableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let reader be a new ReadableStreamBYOBReader.
    auto reader = realm.heap().allocate<ReadableStreamBYOBReader>(realm, realm);

    // 2. Perform ? SetUpReadableStreamBYOBReader(reader, stream).
    TRY(set_up_readable_stream_byob_reader(reader, stream));

    // 3. Return reader.
    return reader;
}

// https://streams.spec.whatwg.org/#is-readable-stream-locked
bool is_readable_stream_locked(ReadableStream const& stream)
{
    // 1. If stream.[[reader]] is undefined, return false.
    if (!stream.reader().has_value())
        return false;

    // 2. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#readable-stream-cancel
JS::NonnullGCPtr<WebIDL::Promise> readable_stream_cancel(ReadableStream& stream, JS::Value reason)
{
    auto& realm = stream.realm();

    // 1. Set stream.[[disturbed]] to true.
    stream.set_disturbed(true);

    // 2. If stream.[[state]] is "closed", return a promise resolved with undefined.
    if (stream.state() == ReadableStream::State::Closed)
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 3. If stream.[[state]] is "errored", return a promise rejected with stream.[[storedError]].
    if (stream.state() == ReadableStream::State::Errored)
        return WebIDL::create_rejected_promise(realm, stream.stored_error());

    // 4. Perform ! ReadableStreamClose(stream).
    readable_stream_close(stream);

    // 5. Let reader be stream.[[reader]].
    auto reader = stream.reader();

    // 6. If reader is not undefined and reader implements ReadableStreamBYOBReader,
    if (reader.has_value() && reader->has<JS::NonnullGCPtr<ReadableStreamBYOBReader>>()) {
        // 1. Let readIntoRequests be reader.[[readIntoRequests]].
        // 2. Set reader.[[readIntoRequests]] to an empty list.
        auto read_into_requests = move(reader->get<JS::NonnullGCPtr<ReadableStreamBYOBReader>>()->read_into_requests());

        // 3. For each readIntoRequest of readIntoRequests,
        for (auto& read_into_request : read_into_requests) {
            // 1. Perform readIntoRequest’s close steps, given undefined.
            read_into_request->on_close(JS::js_undefined());
        }
    }

    // 7. Let sourceCancelPromise be ! stream.[[controller]].[[CancelSteps]](reason).
    auto source_cancel_promise = stream.controller()->visit([&](auto const& controller) {
        return controller->cancel_steps(reason);
    });

    // 8. Return the result of reacting to sourceCancelPromise with a fulfillment step that returns undefined.
    auto react_result = WebIDL::react_to_promise(*source_cancel_promise,
        JS::create_heap_function(stream.heap(), [](JS::Value) -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); }),
        {});

    return WebIDL::create_resolved_promise(realm, react_result);
}

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-into-request
void readable_stream_fulfill_read_into_request(ReadableStream& stream, JS::Value chunk, bool done)
{
    // 1. Assert: ! ReadableStreamHasBYOBReader(stream) is true.
    VERIFY(readable_stream_has_byob_reader(stream));

    // 2. Let reader be stream.[[reader]].
    auto reader = stream.reader()->get<JS::NonnullGCPtr<ReadableStreamBYOBReader>>();

    // 3. Assert: reader.[[readIntoRequests]] is not empty.
    VERIFY(!reader->read_into_requests().is_empty());

    // 4. Let readIntoRequest be reader.[[readIntoRequests]][0].
    // 5. Remove readIntoRequest from reader.[[readIntoRequests]].
    auto read_into_request = reader->read_into_requests().take_first();

    // 6. If done is true, perform readIntoRequest’s close steps, given chunk.
    if (done) {
        read_into_request->on_close(chunk);
    }
    // 7. Otherwise, perform readIntoRequest’s chunk steps, given chunk.
    else {
        read_into_request->on_chunk(chunk);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-request
void readable_stream_fulfill_read_request(ReadableStream& stream, JS::Value chunk, bool done)
{
    // 1. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
    VERIFY(readable_stream_has_default_reader(stream));

    // 2. Let reader be stream.[[reader]].
    auto reader = stream.reader()->get<JS::NonnullGCPtr<ReadableStreamDefaultReader>>();

    // 3. Assert: reader.[[readRequests]] is not empty.
    VERIFY(!reader->read_requests().is_empty());

    // 4. Let readRequest be reader.[[readRequests]][0].
    // 5. Remove readRequest from reader.[[readRequests]].
    auto read_request = reader->read_requests().take_first();

    // 6. If done is true, perform readRequest’s close steps.
    if (done) {
        read_request->on_close();
    }
    // 7. Otherwise, perform readRequest’s chunk steps, given chunk.
    else {
        read_request->on_chunk(chunk);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-into-requests
size_t readable_stream_get_num_read_into_requests(ReadableStream const& stream)
{
    // 1. Assert: ! ReadableStreamHasBYOBReader(stream) is true.
    VERIFY(readable_stream_has_byob_reader(stream));

    // 2. Return stream.[[reader]].[[readIntoRequests]]'s size.
    return stream.reader()->get<JS::NonnullGCPtr<ReadableStreamBYOBReader>>()->read_into_requests().size();
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-requests
size_t readable_stream_get_num_read_requests(ReadableStream const& stream)
{
    // 1. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
    VERIFY(readable_stream_has_default_reader(stream));

    // 2. Return stream.[[reader]].[[readRequests]]'s size.
    return stream.reader()->get<JS::NonnullGCPtr<ReadableStreamDefaultReader>>()->read_requests().size();
}

// https://streams.spec.whatwg.org/#readable-stream-has-byob-reader
bool readable_stream_has_byob_reader(ReadableStream const& stream)
{
    // 1. Let reader be stream.[[reader]].
    auto reader = stream.reader();

    // 2. If reader is undefined, return false.
    if (!reader.has_value())
        return false;

    // 3. If reader implements ReadableStreamBYOBReader, return true.
    if (reader->has<JS::NonnullGCPtr<ReadableStreamBYOBReader>>())
        return true;

    // 4. Return false.
    return false;
}

// https://streams.spec.whatwg.org/#readable-stream-has-default-reader
bool readable_stream_has_default_reader(ReadableStream const& stream)
{
    // 1. Let reader be stream.[[reader]].
    auto reader = stream.reader();

    // 2. If reader is undefined, return false.
    if (!reader.has_value())
        return false;

    // 3. If reader implements ReadableStreamDefaultReader, return true.
    if (reader->has<JS::NonnullGCPtr<ReadableStreamDefaultReader>>())
        return true;

    // 4. Return false.
    return false;
}

// https://streams.spec.whatwg.org/#readable-stream-pipe-to
JS::NonnullGCPtr<WebIDL::Promise> readable_stream_pipe_to(ReadableStream& source, WritableStream& dest, bool, bool, bool, Optional<JS::Value> signal)
{
    auto& realm = source.realm();

    // 1. Assert: source implements ReadableStream.
    // 2. Assert: dest implements WritableStream.
    // 3. Assert: preventClose, preventAbort, and preventCancel are all booleans.

    // 4. If signal was not given, let signal be undefined.
    if (!signal.has_value())
        signal = JS::js_undefined();

    // 5. Assert: either signal is undefined, or signal implements AbortSignal.
    VERIFY(signal->is_undefined() || (signal->is_object() && is<DOM::AbortSignal>(signal->as_object())));

    // 6. Assert: ! IsReadableStreamLocked(source) is false.
    VERIFY(!is_readable_stream_locked(source));

    // 7. Assert: ! IsWritableStreamLocked(dest) is false.
    VERIFY(!is_writable_stream_locked(dest));

    // 8. If source.[[controller]] implements ReadableByteStreamController, let reader be either ! AcquireReadableStreamBYOBReader(source)
    //    or ! AcquireReadableStreamDefaultReader(source), at the user agent’s discretion.
    // 9. Otherwise, let reader be ! AcquireReadableStreamDefaultReader(source).
    auto reader = MUST(source.controller()->visit(
        [](auto const& controller) {
            return acquire_readable_stream_default_reader(*controller->stream());
        }));

    // 10. Let writer be ! AcquireWritableStreamDefaultWriter(dest).
    auto writer = MUST(acquire_writable_stream_default_writer(dest));

    // 11. Set source.[[disturbed]] to true.
    source.set_disturbed(true);

    // FIXME: 12. Let shuttingDown be false.

    // 13. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // FIXME 14. If signal is not undefined,
    //           1. Let abortAlgorithm be the following steps:
    //              1. Let error be signal’s abort reason.
    //              2. Let actions be an empty ordered set.
    //              3. If preventAbort is false, append the following action to actions:
    //                 1. If dest.[[state]] is "writable", return ! WritableStreamAbort(dest, error).
    //                 2. Otherwise, return a promise resolved with undefined.
    //              4. If preventCancel is false, append the following action to actions:
    //                 1. If source.[[state]] is "readable", return ! ReadableStreamCancel(source, error).
    //                 2. Otherwise, return a promise resolved with undefined.
    //              5. Shutdown with an action consisting of getting a promise to wait for all of the actions in actions, and with error.
    //           2. If signal is aborted, perform abortAlgorithm and return promise.
    //           3. Add abortAlgorithm to signal.

    // 15. In parallel but not really; see #905, using reader and writer, read all chunks from source and write them to
    //     dest. Due to the locking provided by the reader and writer, the exact manner in which this happens is not
    //     observable to author code, and so there is flexibility in how this is done. The following constraints apply
    //     regardless of the exact algorithm used:
    //     - Public API must not be used: while reading or writing, or performing any of the operations below, the
    //       JavaScript-modifiable reader, writer, and stream APIs (i.e. methods on the appropriate prototypes) must not
    //       be used. Instead, the streams must be manipulated directly.

    // FIXME: Currently a naive implementation that uses ReadableStreamDefaultReader::read_all_chunks() to read all chunks
    //        from the source and then through the callback success_steps writes those chunks to the destination.
    auto chunk_steps = [&realm, writer](ByteBuffer buffer) {
        auto array_buffer = JS::ArrayBuffer::create(realm, move(buffer));
        auto chunk = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

        auto promise = writable_stream_default_writer_write(writer, chunk);
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());
    };

    auto success_steps = [promise, &realm, writer](ByteBuffer) {
        // Make sure we close the acquired writer.
        WebIDL::resolve_promise(realm, writable_stream_default_writer_close(*writer), JS::js_undefined());

        WebIDL::resolve_promise(realm, promise, JS::js_undefined());
    };

    auto failure_steps = [promise, &realm, writer](JS::Value error) {
        // Make sure we close the acquired writer.
        WebIDL::resolve_promise(realm, writable_stream_default_writer_close(*writer), JS::js_undefined());

        WebIDL::reject_promise(realm, promise, error);
    };

    reader->read_all_chunks(move(chunk_steps), move(success_steps), move(failure_steps));

    // 16. Return promise.
    return promise;
}

// https://streams.spec.whatwg.org/#readable-stream-tee
WebIDL::ExceptionOr<ReadableStreamPair> readable_stream_tee(JS::Realm& realm, ReadableStream& stream, bool clone_for_branch2)
{
    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: cloneForBranch2 is a boolean.

    // 3. If stream.[[controller]] implements ReadableByteStreamController, return ? ReadableByteStreamTee(stream).
    if (stream.controller()->has<JS::NonnullGCPtr<Streams::ReadableByteStreamController>>()) {
        return TRY(readable_byte_stream_tee(realm, stream));
    }

    // 4. Return ? ReadableStreamDefaultTee(stream, cloneForBranch2).
    return TRY(readable_stream_default_tee(realm, stream, clone_for_branch2));
}

struct DefaultStreamTeeParams final : JS::Cell {
    JS_CELL(DefaultStreamTeeParams, JS::Cell);
    JS_DECLARE_ALLOCATOR(DefaultStreamTeeParams);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(reason1);
        visitor.visit(reason2);
        visitor.visit(branch1);
        visitor.visit(branch2);
        visitor.visit(pull_algorithm);
    }

    bool reading { false };
    bool read_again { false };
    bool canceled1 { false };
    bool canceled2 { false };
    JS::Value reason1 { JS::js_undefined() };
    JS::Value reason2 { JS::js_undefined() };
    JS::GCPtr<ReadableStream> branch1;
    JS::GCPtr<ReadableStream> branch2;
    JS::GCPtr<PullAlgorithm> pull_algorithm;
};

JS_DEFINE_ALLOCATOR(DefaultStreamTeeParams);

// https://streams.spec.whatwg.org/#ref-for-read-request③
class DefaultStreamTeeReadRequest final : public ReadRequest {
    JS_CELL(DefaultStreamTeeReadRequest, ReadRequest);
    JS_DECLARE_ALLOCATOR(DefaultStreamTeeReadRequest);

public:
    DefaultStreamTeeReadRequest(
        JS::Realm& realm,
        JS::NonnullGCPtr<ReadableStream> stream,
        JS::NonnullGCPtr<DefaultStreamTeeParams> params,
        JS::NonnullGCPtr<WebIDL::Promise> cancel_promise,
        bool clone_for_branch2)
        : m_realm(realm)
        , m_stream(stream)
        , m_params(params)
        , m_cancel_promise(cancel_promise)
        , m_clone_for_branch2(clone_for_branch2)
    {
    }

    // https://streams.spec.whatwg.org/#ref-for-read-request-chunk-steps③
    virtual void on_chunk(JS::Value chunk) override
    {
        // 1. Queue a microtask to perform the following steps:
        HTML::queue_a_microtask(nullptr, JS::create_heap_function(m_realm->heap(), [this, chunk]() {
            HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(m_realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            auto controller1 = m_params->branch1->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();
            auto controller2 = m_params->branch2->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();

            // 1. Set readAgain to false.
            m_params->read_again = false;

            // 2. Let chunk1 and chunk2 be chunk.
            auto chunk1 = chunk;
            auto chunk2 = chunk;

            // 3. If canceled2 is false and cloneForBranch2 is true,
            if (!m_params->canceled2 && m_clone_for_branch2) {
                // 1. Let cloneResult be StructuredClone(chunk2).
                auto clone_result = structured_clone(m_realm, chunk2);

                // 2. If cloneResult is an abrupt completion,
                if (clone_result.is_exception()) {
                    auto completion = Bindings::dom_exception_to_throw_completion(m_realm->vm(), clone_result.release_error());

                    // 1. Perform ! ReadableStreamDefaultControllerError(branch1.[[controller]], cloneResult.[[Value]]).
                    readable_stream_default_controller_error(controller1, completion.value().value());

                    // 2. Perform ! ReadableStreamDefaultControllerError(branch2.[[controller]], cloneResult.[[Value]]).
                    readable_stream_default_controller_error(controller2, completion.value().value());

                    // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                    auto cancel_result = readable_stream_cancel(m_stream, completion.value().value());
                    JS::NonnullGCPtr cancel_value = verify_cast<JS::Promise>(*cancel_result->promise().ptr());

                    WebIDL::resolve_promise(m_realm, m_cancel_promise, cancel_value);

                    // 4. Return.
                    return;
                }

                // 3. Otherwise, set chunk2 to cloneResult.[[Value]].
                chunk2 = clone_result.release_value();
            }

            // 4. If canceled1 is false, perform ! ReadableStreamDefaultControllerEnqueue(branch1.[[controller]], chunk1).
            if (!m_params->canceled1) {
                MUST(readable_stream_default_controller_enqueue(controller1, chunk1));
            }

            // 5. If canceled2 is false, perform ! ReadableStreamDefaultControllerEnqueue(branch2.[[controller]], chunk2).
            if (!m_params->canceled2) {
                MUST(readable_stream_default_controller_enqueue(controller2, chunk2));
            }

            // 6. Set reading to false.
            m_params->reading = false;

            // 7. If readAgain is true, perform pullAlgorithm.
            if (m_params->read_again) {
                m_params->pull_algorithm->function()();
            }
        }));

        // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
        //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
        //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
    }

    // https://streams.spec.whatwg.org/#ref-for-read-request-close-steps②
    virtual void on_close() override
    {
        auto controller1 = m_params->branch1->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();
        auto controller2 = m_params->branch2->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();

        // 1. Set reading to false.
        m_params->reading = false;

        // 2. If canceled1 is false, perform ! ReadableStreamDefaultControllerClose(branch1.[[controller]]).
        if (!m_params->canceled1) {
            readable_stream_default_controller_close(controller1);
        }

        // 3. If canceled2 is false, perform ! ReadableStreamDefaultControllerClose(branch2.[[controller]]).
        if (!m_params->canceled2) {
            readable_stream_default_controller_close(controller2);
        }

        // 4. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
        if (!m_params->canceled1 || !m_params->canceled2) {
            WebIDL::resolve_promise(m_realm, m_cancel_promise, JS::js_undefined());
        }
    }

    // https://streams.spec.whatwg.org/#ref-for-read-request-error-steps③
    virtual void on_error(JS::Value) override
    {
        // 1. Set reading to false.
        m_params->reading = false;
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_stream);
        visitor.visit(m_params);
        visitor.visit(m_cancel_promise);
    }

    JS::NonnullGCPtr<JS::Realm> m_realm;
    JS::NonnullGCPtr<ReadableStream> m_stream;
    JS::NonnullGCPtr<DefaultStreamTeeParams> m_params;
    JS::NonnullGCPtr<WebIDL::Promise> m_cancel_promise;
    bool m_clone_for_branch2 { false };
};

JS_DEFINE_ALLOCATOR(DefaultStreamTeeReadRequest);

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaulttee
WebIDL::ExceptionOr<ReadableStreamPair> readable_stream_default_tee(JS::Realm& realm, ReadableStream& stream, bool clone_for_branch2)
{
    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: cloneForBranch2 is a boolean.

    // 3. Let reader be ? AcquireReadableStreamDefaultReader(stream).
    auto reader = TRY(acquire_readable_stream_default_reader(stream));

    // 4. Let reading be false.
    // 5. Let readAgain be false.
    // 6. Let canceled1 be false.
    // 7. Let canceled2 be false.
    // 8. Let reason1 be undefined.
    // 9. Let reason2 be undefined.
    // 10. Let branch1 be undefined.
    // 11. Let branch2 be undefined.
    auto params = realm.heap().allocate<DefaultStreamTeeParams>(realm);

    // 12. Let cancelPromise be a new promise.
    auto cancel_promise = WebIDL::create_promise(realm);

    // 13. Let pullAlgorithm be the following steps:
    auto pull_algorithm = JS::create_heap_function(realm.heap(), [&realm, &stream, reader, params, cancel_promise, clone_for_branch2]() {
        // 1. If reading is true,
        if (params->reading) {
            // 1. Set readAgain to true.
            params->read_again = true;

            // 2. Return a promise resolved with undefined.
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());
        }

        // 2. Set reading to true.
        params->reading = true;

        // 3. Let readRequest be a read request with the following items:
        auto read_request = realm.heap().allocate_without_realm<DefaultStreamTeeReadRequest>(realm, stream, params, cancel_promise, clone_for_branch2);

        // 4. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        readable_stream_default_reader_read(reader, read_request);

        // 5. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // AD-HOC: The read request within the pull algorithm must be able to re-invoke the pull algorithm, so cache it here.
    params->pull_algorithm = pull_algorithm;

    // 14. Let cancel1Algorithm be the following steps, taking a reason argument:
    auto cancel1_algorithm = JS::create_heap_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
        // 1. Set canceled1 to true.
        params->canceled1 = true;

        // 2. Set reason1 to reason.
        params->reason1 = reason;

        // 3. If canceled2 is true,
        if (params->canceled2) {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            auto composite_reason = JS::Array::create_from(realm, AK::Array { params->reason1, params->reason2 });

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            auto cancel_result = readable_stream_cancel(stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            JS::NonnullGCPtr cancel_value = verify_cast<JS::Promise>(*cancel_result->promise().ptr());
            WebIDL::resolve_promise(realm, cancel_promise, cancel_value);
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 15. Let cancel2Algorithm be the following steps, taking a reason argument:
    auto cancel2_algorithm = JS::create_heap_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
        // 1. Set canceled2 to true.
        params->canceled2 = true;

        // 2. Set reason2 to reason.
        params->reason2 = reason;

        // 3. If canceled1 is true,
        if (params->canceled1) {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            auto composite_reason = JS::Array::create_from(realm, AK::Array { params->reason1, params->reason2 });

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            auto cancel_result = readable_stream_cancel(stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            JS::NonnullGCPtr cancel_value = verify_cast<JS::Promise>(*cancel_result->promise().ptr());
            WebIDL::resolve_promise(realm, cancel_promise, cancel_value);
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 16. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = JS::create_heap_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 17. Set branch1 to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancel1Algorithm).
    params->branch1 = MUST(create_readable_stream(realm, start_algorithm, pull_algorithm, cancel1_algorithm));

    // 18. Set branch2 to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancel2Algorithm).
    params->branch2 = MUST(create_readable_stream(realm, start_algorithm, pull_algorithm, cancel2_algorithm));

    // 19. Upon rejection of reader.[[closedPromise]] with reason r,
    WebIDL::upon_rejection(*reader->closed_promise_capability(), JS::create_heap_function(realm.heap(), [&realm, params, cancel_promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        auto controller1 = params->branch1->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();
        auto controller2 = params->branch2->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();

        // 1. Perform ! ReadableStreamDefaultControllerError(branch1.[[controller]], r).
        readable_stream_default_controller_error(controller1, reason);

        // 2. Perform ! ReadableStreamDefaultControllerError(branch2.[[controller]], r).
        readable_stream_default_controller_error(controller2, reason);

        // 3. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
        if (!params->canceled1 || !params->canceled2) {
            WebIDL::resolve_promise(realm, cancel_promise, JS::js_undefined());
        }

        return JS::js_undefined();
    }));

    // 20. Return « branch1, branch2 ».
    return ReadableStreamPair { *params->branch1, *params->branch2 };
}

struct ByteStreamTeeParams final : JS::Cell {
    JS_CELL(ByteStreamTeeParams, JS::Cell);
    JS_DECLARE_ALLOCATOR(ByteStreamTeeParams);

    explicit ByteStreamTeeParams(ReadableStreamReader reader)
        : reader(move(reader))
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(reason1);
        visitor.visit(reason2);
        visitor.visit(branch1);
        visitor.visit(branch2);
        visitor.visit(pull1_algorithm);
        visitor.visit(pull2_algorithm);
        reader.visit([&](auto const& underlying_reader) { visitor.visit(underlying_reader); });
    }

    bool reading { false };
    bool read_again_for_branch1 { false };
    bool read_again_for_branch2 { false };
    bool canceled1 { false };
    bool canceled2 { false };
    JS::Value reason1 { JS::js_undefined() };
    JS::Value reason2 { JS::js_undefined() };
    JS::GCPtr<ReadableStream> branch1;
    JS::GCPtr<ReadableStream> branch2;
    JS::GCPtr<PullAlgorithm> pull1_algorithm;
    JS::GCPtr<PullAlgorithm> pull2_algorithm;
    ReadableStreamReader reader;
};

JS_DEFINE_ALLOCATOR(ByteStreamTeeParams);

// https://streams.spec.whatwg.org/#ref-for-read-request④
class ByteStreamTeeDefaultReadRequest final : public ReadRequest {
    JS_CELL(ByteStreamTeeDefaultReadRequest, ReadRequest);
    JS_DECLARE_ALLOCATOR(ByteStreamTeeDefaultReadRequest);

public:
    ByteStreamTeeDefaultReadRequest(
        JS::Realm& realm,
        JS::NonnullGCPtr<ReadableStream> stream,
        JS::NonnullGCPtr<ByteStreamTeeParams> params,
        JS::NonnullGCPtr<WebIDL::Promise> cancel_promise)
        : m_realm(realm)
        , m_stream(stream)
        , m_params(params)
        , m_cancel_promise(cancel_promise)
    {
    }

    // https://streams.spec.whatwg.org/#ref-for-read-request-chunk-steps④
    virtual void on_chunk(JS::Value chunk) override
    {
        // 1. Queue a microtask to perform the following steps:
        HTML::queue_a_microtask(nullptr, JS::create_heap_function(m_realm->heap(), [this, chunk]() mutable {
            HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(m_realm) };

            auto controller1 = m_params->branch1->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();
            auto controller2 = m_params->branch2->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

            // 1. Set readAgainForBranch1 to false.
            m_params->read_again_for_branch1 = false;

            // 2. Set readAgainForBranch2 to false.
            m_params->read_again_for_branch2 = false;

            // 3. Let chunk1 and chunk2 be chunk.
            auto chunk1 = chunk;
            auto chunk2 = chunk;

            // 4. If canceled1 is false and canceled2 is false,
            if (!m_params->canceled1 && !m_params->canceled2) {
                // 1. Let cloneResult be CloneAsUint8Array(chunk).
                auto chunk_view = m_realm->vm().heap().allocate<WebIDL::ArrayBufferView>(m_realm, chunk.as_object());
                auto clone_result = clone_as_uint8_array(m_realm, chunk_view);

                // 2. If cloneResult is an abrupt completion,
                if (clone_result.is_exception()) {
                    auto completion = Bindings::dom_exception_to_throw_completion(m_realm->vm(), clone_result.release_error());

                    // 1. Perform ! ReadableByteStreamControllerError(branch1.[[controller]], cloneResult.[[Value]]).
                    readable_byte_stream_controller_error(controller1, completion.value().value());

                    // 2. Perform ! ReadableByteStreamControllerError(branch2.[[controller]], cloneResult.[[Value]]).
                    readable_byte_stream_controller_error(controller2, completion.value().value());

                    // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                    auto cancel_result = readable_stream_cancel(m_stream, completion.value().value());
                    JS::NonnullGCPtr cancel_value = verify_cast<JS::Promise>(*cancel_result->promise().ptr());

                    WebIDL::resolve_promise(m_realm, m_cancel_promise, cancel_value);

                    // 4. Return.
                    return;
                }

                // 3. Otherwise, set chunk2 to cloneResult.[[Value]].
                chunk2 = clone_result.release_value();
            }

            // 5. If canceled1 is false, perform ! ReadableByteStreamControllerEnqueue(branch1.[[controller]], chunk1).
            if (!m_params->canceled1) {
                MUST(readable_byte_stream_controller_enqueue(controller1, chunk1));
            }

            // 6. If canceled2 is false, perform ! ReadableByteStreamControllerEnqueue(branch2.[[controller]], chunk2).
            if (!m_params->canceled2) {
                MUST(readable_byte_stream_controller_enqueue(controller2, chunk2));
            }

            // 7. Set reading to false.
            m_params->reading = false;

            // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
            if (m_params->read_again_for_branch1) {
                m_params->pull1_algorithm->function()();
            }
            // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
            else if (m_params->read_again_for_branch2) {
                m_params->pull2_algorithm->function()();
            }
        }));

        // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
        //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
        //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
    }

    // https://streams.spec.whatwg.org/#ref-for-read-request-close-steps③
    virtual void on_close() override
    {
        auto controller1 = m_params->branch1->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();
        auto controller2 = m_params->branch2->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

        // 1. Set reading to false.
        m_params->reading = false;

        // 2. If canceled1 is false, perform ! ReadableByteStreamControllerClose(branch1.[[controller]]).
        if (!m_params->canceled1) {
            MUST(readable_byte_stream_controller_close(controller1));
        }

        // 3. If canceled2 is false, perform ! ReadableByteStreamControllerClose(branch2.[[controller]]).
        if (!m_params->canceled2) {
            MUST(readable_byte_stream_controller_close(controller2));
        }

        // 4. If branch1.[[controller]].[[pendingPullIntos]] is not empty, perform ! ReadableByteStreamControllerRespond(branch1.[[controller]], 0).
        if (!controller1->pending_pull_intos().is_empty()) {
            MUST(readable_byte_stream_controller_respond(controller1, 0));
        }

        // 5. If branch2.[[controller]].[[pendingPullIntos]] is not empty, perform ! ReadableByteStreamControllerRespond(branch2.[[controller]], 0).
        if (!controller2->pending_pull_intos().is_empty()) {
            MUST(readable_byte_stream_controller_respond(controller2, 0));
        }

        // 6. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
        if (!m_params->canceled1 || !m_params->canceled2) {
            WebIDL::resolve_promise(m_realm, m_cancel_promise, JS::js_undefined());
        }
    }

    // https://streams.spec.whatwg.org/#ref-for-read-request-error-steps④
    virtual void on_error(JS::Value) override
    {
        // 1. Set reading to false.
        m_params->reading = false;
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_stream);
        visitor.visit(m_params);
        visitor.visit(m_cancel_promise);
    }

    JS::NonnullGCPtr<JS::Realm> m_realm;
    JS::NonnullGCPtr<ReadableStream> m_stream;
    JS::NonnullGCPtr<ByteStreamTeeParams> m_params;
    JS::NonnullGCPtr<WebIDL::Promise> m_cancel_promise;
};

JS_DEFINE_ALLOCATOR(ByteStreamTeeDefaultReadRequest);

// https://streams.spec.whatwg.org/#ref-for-read-into-request②
class ByteStreamTeeBYOBReadRequest final : public ReadIntoRequest {
    JS_CELL(ByteStreamTeeBYOBReadRequest, ReadIntoRequest);
    JS_DECLARE_ALLOCATOR(ByteStreamTeeBYOBReadRequest);

public:
    ByteStreamTeeBYOBReadRequest(
        JS::Realm& realm,
        JS::NonnullGCPtr<ReadableStream> stream,
        JS::NonnullGCPtr<ByteStreamTeeParams> params,
        JS::NonnullGCPtr<WebIDL::Promise> cancel_promise,
        JS::NonnullGCPtr<ReadableStream> byob_branch,
        JS::NonnullGCPtr<ReadableStream> other_branch,
        bool for_branch2)
        : m_realm(realm)
        , m_stream(stream)
        , m_params(params)
        , m_cancel_promise(cancel_promise)
        , m_byob_branch(byob_branch)
        , m_other_branch(other_branch)
        , m_for_branch2(for_branch2)
    {
    }

    // https://streams.spec.whatwg.org/#ref-for-read-into-request-chunk-steps①
    virtual void on_chunk(JS::Value chunk) override
    {
        auto chunk_view = m_realm->vm().heap().allocate<WebIDL::ArrayBufferView>(m_realm, chunk.as_object());

        // 1. Queue a microtask to perform the following steps:
        HTML::queue_a_microtask(nullptr, JS::create_heap_function(m_realm->heap(), [this, chunk = chunk_view]() {
            HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(m_realm) };

            auto byob_controller = m_byob_branch->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();
            auto other_controller = m_other_branch->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

            // 1. Set readAgainForBranch1 to false.
            m_params->read_again_for_branch1 = false;

            // 2. Set readAgainForBranch2 to false.
            m_params->read_again_for_branch2 = false;

            // 3. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1 otherwise.
            auto byob_cancelled = m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

            // 4. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1 otherwise.
            auto other_cancelled = !m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

            // 5. If otherCanceled is false,
            if (!other_cancelled) {
                // 1. Let cloneResult be CloneAsUint8Array(chunk).
                auto clone_result = clone_as_uint8_array(m_realm, chunk);

                // 2. If cloneResult is an abrupt completion,
                if (clone_result.is_exception()) {
                    auto completion = Bindings::dom_exception_to_throw_completion(m_realm->vm(), clone_result.release_error());

                    // 1. Perform ! ReadableByteStreamControllerError(byobBranch.[[controller]], cloneResult.[[Value]]).
                    readable_byte_stream_controller_error(byob_controller, completion.value().value());

                    // 2. Perform ! ReadableByteStreamControllerError(otherBranch.[[controller]], cloneResult.[[Value]]).
                    readable_byte_stream_controller_error(other_controller, completion.value().value());

                    // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                    auto cancel_result = readable_stream_cancel(m_stream, completion.value().value());
                    JS::NonnullGCPtr cancel_value = verify_cast<JS::Promise>(*cancel_result->promise().ptr());

                    WebIDL::resolve_promise(m_realm, m_cancel_promise, cancel_value);

                    // 4. Return.
                    return;
                }

                // 3. Otherwise, let clonedChunk be cloneResult.[[Value]].
                auto cloned_chunk = clone_result.release_value();

                // 4. If byobCanceled is false, perform ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
                if (!byob_cancelled) {
                    MUST(readable_byte_stream_controller_respond_with_new_view(m_realm, byob_controller, chunk));
                }

                // 5. Perform ! ReadableByteStreamControllerEnqueue(otherBranch.[[controller]], clonedChunk).
                MUST(readable_byte_stream_controller_enqueue(other_controller, cloned_chunk));
            }
            // 6. Otherwise, if byobCanceled is false, perform ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
            else if (!byob_cancelled) {
                MUST(readable_byte_stream_controller_respond_with_new_view(m_realm, byob_controller, chunk));
            }

            // 7. Set reading to false.
            m_params->reading = false;

            // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
            if (m_params->read_again_for_branch1) {
                m_params->pull1_algorithm->function()();
            }
            // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
            else if (m_params->read_again_for_branch2) {
                m_params->pull2_algorithm->function()();
            }
        }));

        // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
        //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
        //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
    }

    // https://streams.spec.whatwg.org/#ref-for-read-into-request-close-steps②
    virtual void on_close(JS::Value chunk) override
    {
        auto byob_controller = m_byob_branch->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();
        auto other_controller = m_other_branch->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

        // 1. Set reading to false.
        m_params->reading = false;

        // 2. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1 otherwise.
        auto byob_cancelled = m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

        // 3. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1 otherwise.
        auto other_cancelled = !m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

        // 4. If byobCanceled is false, perform ! ReadableByteStreamControllerClose(byobBranch.[[controller]]).
        if (!byob_cancelled) {
            MUST(readable_byte_stream_controller_close(byob_controller));
        }

        // 5. If otherCanceled is false, perform ! ReadableByteStreamControllerClose(otherBranch.[[controller]]).
        if (!other_cancelled) {
            MUST(readable_byte_stream_controller_close(other_controller));
        }

        // 6. If chunk is not undefined,
        if (!chunk.is_undefined()) {
            // 1. Assert: chunk.[[ByteLength]] is 0.

            // 2. If byobCanceled is false, perform ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
            if (!byob_cancelled) {
                auto array_buffer_view = m_realm->vm().heap().allocate<WebIDL::ArrayBufferView>(m_realm, chunk.as_object());
                MUST(readable_byte_stream_controller_respond_with_new_view(m_realm, byob_controller, array_buffer_view));
            }

            // 3. If otherCanceled is false and otherBranch.[[controller]].[[pendingPullIntos]] is not empty,
            //    perform ! ReadableByteStreamControllerRespond(otherBranch.[[controller]], 0).
            if (!other_cancelled && !other_controller->pending_pull_intos().is_empty()) {
                MUST(readable_byte_stream_controller_respond(other_controller, 0));
            }
        }

        // 7. If byobCanceled is false or otherCanceled is false, resolve cancelPromise with undefined.
        if (!byob_cancelled || !other_cancelled) {
            WebIDL::resolve_promise(m_realm, m_cancel_promise, JS::js_undefined());
        }
    }

    // https://streams.spec.whatwg.org/#ref-for-read-into-request-error-steps①
    virtual void on_error(JS::Value) override
    {
        // 1. Set reading to false.
        m_params->reading = false;
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_stream);
        visitor.visit(m_params);
        visitor.visit(m_cancel_promise);
        visitor.visit(m_byob_branch);
        visitor.visit(m_other_branch);
    }

    JS::NonnullGCPtr<JS::Realm> m_realm;
    JS::NonnullGCPtr<ReadableStream> m_stream;
    JS::NonnullGCPtr<ByteStreamTeeParams> m_params;
    JS::NonnullGCPtr<WebIDL::Promise> m_cancel_promise;
    JS::NonnullGCPtr<ReadableStream> m_byob_branch;
    JS::NonnullGCPtr<ReadableStream> m_other_branch;
    bool m_for_branch2 { false };
};

JS_DEFINE_ALLOCATOR(ByteStreamTeeBYOBReadRequest);

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
WebIDL::ExceptionOr<ReadableStreamPair> readable_byte_stream_tee(JS::Realm& realm, ReadableStream& stream)
{
    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: stream.[[controller]] implements ReadableByteStreamController.
    VERIFY(stream.controller().has_value() && stream.controller()->has<JS::NonnullGCPtr<ReadableByteStreamController>>());

    // 3. Let reader be ? AcquireReadableStreamDefaultReader(stream).
    auto reader = TRY(acquire_readable_stream_default_reader(stream));

    // 4. Let reading be false.
    // 5. Let readAgainForBranch1 be false.
    // 6. Let readAgainForBranch2 be false.
    // 7. Let canceled1 be false.
    // 8. Let canceled2 be false.
    // 9. Let reason1 be undefined.
    // 10. Let reason2 be undefined.
    // 11. Let branch1 be undefined.
    // 12. Let branch2 be undefined.
    auto params = realm.heap().allocate<ByteStreamTeeParams>(realm, reader);

    // 13. Let cancelPromise be a new promise.
    auto cancel_promise = WebIDL::create_promise(realm);

    // 14. Let forwardReaderError be the following steps, taking a thisReader argument:
    auto forward_reader_error = JS::create_heap_function(realm.heap(), [&realm, params, cancel_promise](ReadableStreamReader const& this_reader) {
        // 1. Upon rejection of thisReader.[[closedPromise]] with reason r,
        auto closed_promise = this_reader.visit([](auto const& underlying_reader) { return underlying_reader->closed_promise_capability(); });

        WebIDL::upon_rejection(*closed_promise, JS::create_heap_function(realm.heap(), [&realm, this_reader, params, cancel_promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            auto controller1 = params->branch1->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();
            auto controller2 = params->branch2->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

            // 1. If thisReader is not reader, return.
            if (this_reader != params->reader) {
                return JS::js_undefined();
            }

            // 2. Perform ! ReadableByteStreamControllerError(branch1.[[controller]], r).
            readable_byte_stream_controller_error(controller1, reason);

            // 3. Perform ! ReadableByteStreamControllerError(branch2.[[controller]], r).
            readable_byte_stream_controller_error(controller2, reason);

            // 4. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
            if (!params->canceled1 || !params->canceled2) {
                WebIDL::resolve_promise(realm, cancel_promise, JS::js_undefined());
            }

            return JS::js_undefined();
        }));
    });

    // 15. Let pullWithDefaultReader be the following steps:
    auto pull_with_default_reader = JS::create_heap_function(realm.heap(), [&realm, &stream, params, cancel_promise, forward_reader_error]() mutable {
        // 1. If reader implements ReadableStreamBYOBReader,
        if (auto const* byob_reader = params->reader.get_pointer<JS::NonnullGCPtr<ReadableStreamBYOBReader>>()) {
            // 1. Assert: reader.[[readIntoRequests]] is empty.
            VERIFY((*byob_reader)->read_into_requests().is_empty());

            // 2. Perform ! ReadableStreamBYOBReaderRelease(reader).
            readable_stream_byob_reader_release(*byob_reader);

            // 3. Set reader to ! AcquireReadableStreamDefaultReader(stream).
            params->reader = MUST(acquire_readable_stream_default_reader(stream));

            // 4. Perform forwardReaderError, given reader.
            forward_reader_error->function()(params->reader);
        }

        // 2. Let readRequest be a read request with the following items:
        auto read_request = realm.heap().allocate_without_realm<ByteStreamTeeDefaultReadRequest>(realm, stream, params, cancel_promise);

        // 3. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        readable_stream_default_reader_read(params->reader.get<JS::NonnullGCPtr<ReadableStreamDefaultReader>>(), read_request);
    });

    // 16. Let pullWithBYOBReader be the following steps, given view and forBranch2:
    auto pull_with_byob_reader = JS::create_heap_function(realm.heap(), [&realm, &stream, params, cancel_promise, forward_reader_error](JS::NonnullGCPtr<WebIDL::ArrayBufferView> view, bool for_branch2) mutable {
        // 1. If reader implements ReadableStreamDefaultReader,
        if (auto const* default_reader = params->reader.get_pointer<JS::NonnullGCPtr<ReadableStreamDefaultReader>>()) {
            // 2. Assert: reader.[[readRequests]] is empty.
            VERIFY((*default_reader)->read_requests().is_empty());

            // 3. Perform ! ReadableStreamDefaultReaderRelease(reader).
            readable_stream_default_reader_release(*default_reader);

            // 4. Set reader to ! AcquireReadableStreamBYOBReader(stream).
            params->reader = MUST(acquire_readable_stream_byob_reader(stream));

            // 5. Perform forwardReaderError, given reader.
            forward_reader_error->function()(params->reader);
        };

        // 2. Let byobBranch be branch2 if forBranch2 is true, and branch1 otherwise.
        auto byob_branch = for_branch2 ? params->branch2 : params->branch1;

        // 3. Let otherBranch be branch2 if forBranch2 is false, and branch1 otherwise.
        auto other_branch = !for_branch2 ? params->branch2 : params->branch1;

        // 4. Let readIntoRequest be a read-into request with the following items:
        auto read_into_request = realm.heap().allocate_without_realm<ByteStreamTeeBYOBReadRequest>(realm, stream, params, cancel_promise, *byob_branch, *other_branch, for_branch2);

        // 5. Perform ! ReadableStreamBYOBReaderRead(reader, view, 1, readIntoRequest).
        readable_stream_byob_reader_read(params->reader.get<JS::NonnullGCPtr<ReadableStreamBYOBReader>>(), view, read_into_request);
    });

    // 17. Let pull1Algorithm be the following steps:
    auto pull1_algorithm = JS::create_heap_function(realm.heap(), [&realm, params, pull_with_default_reader, pull_with_byob_reader]() {
        auto controller1 = params->branch1->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

        // 1. If reading is true,
        if (params->reading) {
            // 1. Set readAgainForBranch1 to true.
            params->read_again_for_branch1 = true;

            // 2. Return a promise resolved with undefined.
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());
        }

        // 2. Set reading to true.
        params->reading = true;

        // 3. Let byobRequest be ! ReadableByteStreamControllerGetBYOBRequest(branch1.[[controller]]).
        auto byob_request = readable_byte_stream_controller_get_byob_request(controller1);

        // 4. If byobRequest is null, perform pullWithDefaultReader.
        if (!byob_request) {
            pull_with_default_reader->function()();
        }
        // 5. Otherwise, perform pullWithBYOBReader, given byobRequest.[[view]] and false.
        else {
            pull_with_byob_reader->function()(*byob_request->view(), false);
        }

        // 6. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 18. Let pull2Algorithm be the following steps:
    auto pull2_algorithm = JS::create_heap_function(realm.heap(), [&realm, params, pull_with_default_reader, pull_with_byob_reader]() {
        auto controller2 = params->branch2->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

        // 1. If reading is true,
        if (params->reading) {
            // 1. Set readAgainForBranch2 to true.
            params->read_again_for_branch2 = true;

            // 2. Return a promise resolved with undefined.
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());
        }

        // 2. Set reading to true.
        params->reading = true;

        // 3. Let byobRequest be ! ReadableByteStreamControllerGetBYOBRequest(branch2.[[controller]]).
        auto byob_request = readable_byte_stream_controller_get_byob_request(controller2);

        // 4. If byobRequest is null, perform pullWithDefaultReader.
        if (!byob_request) {
            pull_with_default_reader->function()();
        }
        // 5. Otherwise, perform pullWithBYOBReader, given byobRequest.[[view]] and true.
        else {
            pull_with_byob_reader->function()(*byob_request->view(), true);
        }

        // 6. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // AD-HOC: The read requests within the pull algorithms must be able to re-invoke the pull algorithms, so cache them here.
    params->pull1_algorithm = pull1_algorithm;
    params->pull2_algorithm = pull2_algorithm;

    // 19. Let cancel1Algorithm be the following steps, taking a reason argument:
    auto cancel1_algorithm = JS::create_heap_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
        // 1. Set canceled1 to true.
        params->canceled1 = true;

        // 2. Set reason1 to reason.
        params->reason1 = reason;

        // 3. If canceled2 is true,
        if (params->canceled2) {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            auto composite_reason = JS::Array::create_from(realm, AK::Array { params->reason1, params->reason2 });

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            auto cancel_result = readable_stream_cancel(stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            JS::NonnullGCPtr cancel_value = verify_cast<JS::Promise>(*cancel_result->promise().ptr());
            WebIDL::resolve_promise(realm, cancel_promise, cancel_value);
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 20. Let cancel2Algorithm be the following steps, taking a reason argument:
    auto cancel2_algorithm = JS::create_heap_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
        // 1. Set canceled2 to true.
        params->canceled2 = true;

        // 2. Set reason2 to reason.
        params->reason2 = reason;

        // 3. If canceled1 is true,
        if (params->canceled1) {
            // 1. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
            auto composite_reason = JS::Array::create_from(realm, AK::Array { params->reason1, params->reason2 });

            // 2. Let cancelResult be ! ReadableStreamCancel(stream, compositeReason).
            auto cancel_result = readable_stream_cancel(stream, composite_reason);

            // 3. Resolve cancelPromise with cancelResult.
            JS::NonnullGCPtr cancel_value = verify_cast<JS::Promise>(*cancel_result->promise().ptr());
            WebIDL::resolve_promise(realm, cancel_promise, cancel_value);
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 21. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = JS::create_heap_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 22. Set branch1 to ! CreateReadableByteStream(startAlgorithm, pull1Algorithm, cancel1Algorithm).
    params->branch1 = MUST(create_readable_byte_stream(realm, start_algorithm, pull1_algorithm, cancel1_algorithm));

    // 23. Set branch2 to ! CreateReadableByteStream(startAlgorithm, pull2Algorithm, cancel2Algorithm).
    params->branch2 = MUST(create_readable_byte_stream(realm, start_algorithm, pull2_algorithm, cancel2_algorithm));

    // 24. Perform forwardReaderError, given reader.
    forward_reader_error->function()(reader);

    // 25. Return « branch1, branch2 ».
    return ReadableStreamPair { *params->branch1, *params->branch2 };
}

// https://streams.spec.whatwg.org/#make-size-algorithm-from-size-function
JS::NonnullGCPtr<SizeAlgorithm> extract_size_algorithm(JS::VM& vm, QueuingStrategy const& strategy)
{
    // 1. If strategy["size"] does not exist, return an algorithm that returns 1.
    if (!strategy.size)
        return JS::create_heap_function(vm.heap(), [](JS::Value) { return JS::normal_completion(JS::Value(1)); });

    // 2. Return an algorithm that performs the following steps, taking a chunk argument:
    return JS::create_heap_function(vm.heap(), [size = strategy.size](JS::Value chunk) {
        return WebIDL::invoke_callback(*size, JS::js_undefined(), chunk);
    });
}

// https://streams.spec.whatwg.org/#validate-and-normalize-high-water-mark
WebIDL::ExceptionOr<double> extract_high_water_mark(QueuingStrategy const& strategy, double default_hwm)
{
    // 1. If strategy["highWaterMark"] does not exist, return defaultHWM.
    if (!strategy.high_water_mark.has_value())
        return default_hwm;

    // 2. Let highWaterMark be strategy["highWaterMark"].
    auto high_water_mark = strategy.high_water_mark.value();

    // 3. If highWaterMark is NaN or highWaterMark < 0, throw a RangeError exception.
    if (isnan(high_water_mark) || high_water_mark < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Invalid value for high water mark"sv };

    // 4. Return highWaterMark.
    return high_water_mark;
}

// https://streams.spec.whatwg.org/#readable-stream-close
void readable_stream_close(ReadableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[state]] is "readable".
    VERIFY(stream.state() == ReadableStream::State::Readable);

    // 2. Set stream.[[state]] to "closed".
    stream.set_state(ReadableStream::State::Closed);

    // 3. Let reader be stream.[[reader]].
    auto reader = stream.reader();

    // 4. If reader is undefined, return.
    if (!reader.has_value())
        return;

    // 5. Resolve reader.[[closedPromise]] with undefined.
    WebIDL::resolve_promise(realm, *reader->visit([](auto& reader) {
        return reader->closed_promise_capability();
    }));

    // 6. If reader implements ReadableStreamDefaultReader,
    if (reader->has<JS::NonnullGCPtr<ReadableStreamDefaultReader>>()) {
        // 1. Let readRequests be reader.[[readRequests]].
        // 2. Set reader.[[readRequests]] to an empty list.
        auto read_requests = move(reader->get<JS::NonnullGCPtr<ReadableStreamDefaultReader>>()->read_requests());

        // 3. For each readRequest of readRequests,
        for (auto& read_request : read_requests) {
            // 1. Perform readRequest’s close steps.
            read_request->on_close();
        }
    }
}

// https://streams.spec.whatwg.org/#readable-stream-error
void readable_stream_error(ReadableStream& stream, JS::Value error)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[state]] is "readable".
    VERIFY(stream.state() == ReadableStream::State::Readable);

    // 2. Set stream.[[state]] to "errored".
    stream.set_state(ReadableStream::State::Errored);

    // 3. Set stream.[[storedError]] to e.
    stream.set_stored_error(error);

    // 4. Let reader be stream.[[reader]].
    auto reader = stream.reader();

    // 5. If reader is undefined, return.
    if (!reader.has_value())
        return;

    auto closed_promise_capability = reader->visit([](auto& reader) { return reader->closed_promise_capability(); });

    // 6. Reject reader.[[closedPromise]] with e.
    WebIDL::reject_promise(realm, *closed_promise_capability, error);

    // 7. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*closed_promise_capability);

    // 8. If reader implements ReadableStreamDefaultReader,
    if (reader->has<JS::NonnullGCPtr<ReadableStreamDefaultReader>>()) {
        // 1. Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e).
        readable_stream_default_reader_error_read_requests(*reader->get<JS::NonnullGCPtr<ReadableStreamDefaultReader>>(), error);
    }
    // 9. Otherwise,
    else {
        // 1. Assert: reader implements ReadableStreamBYOBReader.
        VERIFY(reader->has<JS::NonnullGCPtr<ReadableStreamBYOBReader>>());

        // 2. Perform ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
        readable_stream_byob_reader_error_read_into_requests(*reader->get<JS::NonnullGCPtr<ReadableStreamBYOBReader>>(), error);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-from-iterable
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> readable_stream_from_iterable(JS::VM& vm, JS::Value async_iterable)
{
    auto& realm = *vm.current_realm();

    // 1. Let stream be undefined.
    // NON-STANDARD: We capture 'stream' in a lambda later, so it needs to be allocated now.
    //               'stream' is still in an uninitialized state and will be initialized / set up at step 6.
    auto stream = realm.heap().allocate<ReadableStream>(realm, realm);

    // 2. Let iteratorRecord be ? GetIterator(asyncIterable, async).
    auto iterator_record = TRY(JS::get_iterator(vm, async_iterable, JS::IteratorHint::Async));

    // 3. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = JS::create_heap_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 4. Let pullAlgorithm be the following steps:
    auto pull_algorithm = JS::create_heap_function(realm.heap(), [&vm, &realm, stream, iterator_record]() mutable {
        // 1.  Let nextResult be IteratorNext(iteratorRecord).
        auto next_result = JS::iterator_next(vm, iterator_record);

        // 2. If nextResult is an abrupt completion, return a promise rejected with nextResult.[[Value]].
        if (next_result.is_error())
            return WebIDL::create_rejected_promise(realm, *next_result.throw_completion().release_value());

        // 3. Let nextPromise be a promise resolved with nextResult.[[Value]].
        auto next_promise = WebIDL::create_resolved_promise(realm, next_result.release_value());

        // 4. Return the result of reacting to nextPromise with the following fulfillment steps, given iterResult:
        auto react_result = WebIDL::react_to_promise(*next_promise,
            JS::create_heap_function(realm.heap(), [&vm, stream](JS::Value iter_result) -> WebIDL::ExceptionOr<JS::Value> {
                // 1. If Type(iterResult) is not Object, throw a TypeError.
                if (!iter_result.is_object())
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "iterResult is not an Object"sv };

                // 2. Let done be ? IteratorComplete(iterResult).
                auto done = TRY(JS::iterator_complete(vm, iter_result.as_object()));

                // 3. If done is true:
                if (done) {
                    // 1. Perform ! ReadableStreamDefaultControllerClose(stream.[[controller]]).
                    readable_stream_default_controller_close(*stream->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>());
                }
                // 4. Otherwise:
                else {
                    // 1. Let value be ? IteratorValue(iterResult).
                    auto value = TRY(JS::iterator_value(vm, iter_result.as_object()));

                    // 2. Perform ! ReadableStreamDefaultControllerEnqueue(stream.[[controller]], value).
                    MUST(readable_stream_default_controller_enqueue(*stream->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>(), value));
                }

                return JS::js_undefined();
            }),
            {});

        return WebIDL::create_resolved_promise(realm, react_result);
    });

    // 5. Let cancelAlgorithm be the following steps, given reason:
    auto cancel_algorithm = JS::create_heap_function(realm.heap(), [&vm, &realm, iterator_record](JS::Value reason) {
        // 1. Let iterator be iteratorRecord.[[Iterator]].
        auto iterator = iterator_record->iterator;

        // 2. Let returnMethod be GetMethod(iterator, "return").
        auto return_method = iterator->get(vm.names.return_);

        // 3. If returnMethod is an abrupt completion, return a promise rejected with returnMethod.[[Value]].
        if (return_method.is_error())
            return WebIDL::create_rejected_promise(realm, *return_method.throw_completion().release_value());

        // 4. If returnMethod.[[Value]] is undefined, return a promise resolved with undefined.
        if (return_method.value().is_undefined())
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());

        // 5. Let returnResult be Call(returnMethod.[[Value]], iterator, « reason »).
        auto return_result = JS::call(vm, return_method.value(), reason);

        // 6. If returnResult is an abrupt completion, return a promise rejected with returnResult.[[Value]].
        if (return_result.is_error())
            return WebIDL::create_rejected_promise(realm, *return_result.throw_completion().release_value());

        // 7. Let returnPromise be a promise resolved with returnResult.[[Value]].
        auto return_promise = WebIDL::create_resolved_promise(realm, return_result.release_value());

        // 8. Return the result of reacting to returnPromise with the following fulfillment steps, given iterResult:
        auto react_result = WebIDL::react_to_promise(*return_promise,
            JS::create_heap_function(realm.heap(), [](JS::Value iter_result) -> WebIDL::ExceptionOr<JS::Value> {
                // 1. If Type(iterResult) is not Object, throw a TypeError.
                if (!iter_result.is_object())
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "iterResult is not an Object"sv };

                // 2. Return undefined.
                return JS::js_undefined();
            }),
            {});

        return WebIDL::create_resolved_promise(realm, react_result);
    });

    // 6. Set stream to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, 0).
    // NON-STANDARD: 'stream' is captured in a lambda defined earlier, so we cannot overwrite it by assigning the ReadableStream returned by CreateReadableStream.
    MUST(set_up_readable_stream(realm, *stream, start_algorithm, pull_algorithm, cancel_algorithm, 0));

    // 7. Return stream.
    return stream;
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-request
void readable_stream_add_read_request(ReadableStream& stream, JS::NonnullGCPtr<ReadRequest> read_request)
{
    // 1. Assert: stream.[[reader]] implements ReadableStreamDefaultReader.
    VERIFY(stream.reader().has_value() && stream.reader()->has<JS::NonnullGCPtr<ReadableStreamDefaultReader>>());

    // 2. Assert: stream.[[state]] is "readable".
    VERIFY(stream.state() == ReadableStream::State::Readable);

    // 3. Append readRequest to stream.[[reader]].[[readRequests]].
    stream.reader()->get<JS::NonnullGCPtr<ReadableStreamDefaultReader>>()->read_requests().append(read_request);
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-into-request
void readable_stream_add_read_into_request(ReadableStream& stream, JS::NonnullGCPtr<ReadIntoRequest> read_into_request)
{
    // 1. Assert: stream.[[reader]] implements ReadableStreamBYOBReader.
    VERIFY(stream.reader().has_value() && stream.reader()->has<JS::NonnullGCPtr<ReadableStreamBYOBReader>>());

    // 2. Assert: stream.[[state]] is "readable" or "closed".
    VERIFY(stream.is_readable() || stream.is_closed());

    // 3. Append readRequest to stream.[[reader]].[[readIntoRequests]].
    stream.reader()->get<JS::NonnullGCPtr<ReadableStreamBYOBReader>>()->read_into_requests().append(read_into_request);
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-cancel
JS::NonnullGCPtr<WebIDL::Promise> readable_stream_reader_generic_cancel(ReadableStreamGenericReaderMixin& reader, JS::Value reason)
{
    // 1. Let stream be reader.[[stream]]
    auto stream = reader.stream();

    // 2. Assert: stream is not undefined
    VERIFY(stream);

    // 3. Return ! ReadableStreamCancel(stream, reason)
    return readable_stream_cancel(*stream, reason);
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-initialize
void readable_stream_reader_generic_initialize(ReadableStreamReader reader, ReadableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Set reader.[[stream]] to stream.
    reader.visit([&](auto& reader) { reader->set_stream(stream); });

    // 2. Set stream.[[reader]] to reader.
    stream.set_reader(reader);

    // 3. If stream.[[state]] is "readable",
    if (stream.state() == ReadableStream::State::Readable) {
        // 1. Set reader.[[closedPromise]] to a new promise.
        reader.visit([&](auto& reader) { reader->set_closed_promise_capability(WebIDL::create_promise(realm)); });
    }
    // 4. Otherwise, if stream.[[state]] is "closed",
    else if (stream.state() == ReadableStream::State::Closed) {
        // 1. Set reader.[[closedPromise]] to a promise resolved with undefined.
        reader.visit([&](auto& reader) {
            reader->set_closed_promise_capability(WebIDL::create_resolved_promise(realm, JS::js_undefined()));
        });
    }
    // 5. Otherwise,
    else {
        // 1. Assert: stream.[[state]] is "errored".
        VERIFY(stream.state() == ReadableStream::State::Errored);

        // 2. Set reader.[[closedPromise]] to a promise rejected with stream.[[storedError]].
        // 3. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
        reader.visit([&](auto& reader) {
            reader->set_closed_promise_capability(WebIDL::create_rejected_promise(realm, stream.stored_error()));
            WebIDL::mark_promise_as_handled(*reader->closed_promise_capability());
        });
    }
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-release
void readable_stream_reader_generic_release(ReadableStreamGenericReaderMixin& reader)
{
    // 1. Let stream be reader.[[stream]].
    auto stream = reader.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Assert: stream.[[reader]] is reader.
    VERIFY(stream->reader()->visit([](auto& reader) -> ReadableStreamGenericReaderMixin* { return reader.ptr(); }) == &reader);

    auto& realm = stream->realm();

    // 4. If stream.[[state]] is "readable", reject reader.[[closedPromise]] with a TypeError exception.
    auto exception = JS::TypeError::create(realm, "Released readable stream"sv);
    if (stream->is_readable()) {
        WebIDL::reject_promise(realm, *reader.closed_promise_capability(), exception);
    }
    // 5. Otherwise, set reader.[[closedPromise]] to a promise rejected with a TypeError exception.
    else {
        reader.set_closed_promise_capability(WebIDL::create_rejected_promise(realm, exception));
    }

    // 6. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*reader.closed_promise_capability());

    // 7. Perform ! stream.[[controller]].[[ReleaseSteps]]().
    stream->controller()->visit([](auto const& controller) { return controller->release_steps(); });

    // 8. Set stream.[[reader]] to undefined.
    stream->set_reader({});

    // 9. Set reader.[[stream]] to undefined.
    reader.set_stream({});
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreadererrorreadrequests
void readable_stream_default_reader_error_read_requests(ReadableStreamDefaultReader& reader, JS::Value error)
{
    // 1. Let readRequests be reader.[[readRequests]].
    auto read_requests = move(reader.read_requests());

    // 2. Set reader.[[readRequests]] to a new empty list.
    reader.read_requests().clear();

    // 3. For each readRequest of readRequests,
    for (auto& read_request : read_requests) {
        // 1. Perform readRequest’s error steps, given e.
        read_request->on_error(error);
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreadererrorreadintorequests
void readable_stream_byob_reader_error_read_into_requests(ReadableStreamBYOBReader& reader, JS::Value error)
{
    // 1. Let readIntoRequests be reader.[[readIntoRequests]].
    auto read_into_requests = move(reader.read_into_requests());

    // 2. Set reader.[[readIntoRequests]] to a new empty list.
    reader.read_into_requests().clear();

    // 3. For each readIntoRequest of readIntoRequests,
    for (auto& read_into_request : read_into_requests) {

        // 1. Perform readIntoRequest’s error steps, given e.
        read_into_request->on_error(error);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-head-pull-into-descriptor
void readable_byte_stream_controller_fill_head_pull_into_descriptor(ReadableByteStreamController const& controller, u64 size, PullIntoDescriptor& pull_into_descriptor)
{
    // 1. Assert: either controller.[[pendingPullIntos]] is empty, or controller.[[pendingPullIntos]][0] is pullIntoDescriptor.
    VERIFY(controller.pending_pull_intos().is_empty() || &controller.pending_pull_intos().first() == &pull_into_descriptor);

    // 2. Assert: controller.[[byobRequest]] is null.
    VERIFY(!controller.raw_byob_request());

    // 3. Set pullIntoDescriptor’s bytes filled to bytes filled + size.
    pull_into_descriptor.bytes_filled += size;
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-pull-into-descriptor-from-queue
bool readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(ReadableByteStreamController& controller, PullIntoDescriptor& pull_into_descriptor)
{
    // 1. Let elementSize be pullIntoDescriptor.[[elementSize]].
    auto element_size = pull_into_descriptor.element_size;

    // 2. Let currentAlignedBytes be pullIntoDescriptor’s bytes filled − (pullIntoDescriptor’s bytes filled mod elementSize).
    auto current_aligned_bytes = pull_into_descriptor.bytes_filled - (pull_into_descriptor.bytes_filled % pull_into_descriptor.element_size);

    // 3. Let maxBytesToCopy be min(controller.[[queueTotalSize]], pullIntoDescriptor’s byte length − pullIntoDescriptor’s bytes filled).
    auto max_bytes_to_copy = min(controller.queue_total_size(), pull_into_descriptor.byte_length - pull_into_descriptor.bytes_filled);

    // 4. Let maxBytesFilled be pullIntoDescriptor’s bytes filled + maxBytesToCopy.
    u64 max_bytes_filled = pull_into_descriptor.bytes_filled + max_bytes_to_copy;

    // 5. Let maxAlignedBytes be maxBytesFilled − (maxBytesFilled mod elementSize).
    auto max_aligned_bytes = max_bytes_filled - (max_bytes_filled % element_size);

    // 6. Let totalBytesToCopyRemaining be maxBytesToCopy.
    auto total_bytes_to_copy_remaining = max_bytes_to_copy;

    // 7. Let ready be false.
    bool ready = false;

    // 8. If maxAlignedBytes > currentAlignedBytes,
    if (max_aligned_bytes > current_aligned_bytes) {
        // 1. Set totalBytesToCopyRemaining to maxAlignedBytes − pullIntoDescriptor’s bytes filled.
        total_bytes_to_copy_remaining = max_aligned_bytes - pull_into_descriptor.bytes_filled;

        // 2. Set ready to true.
        ready = true;
    }

    // 9. Let queue be controller.[[queue]].
    auto& queue = controller.queue();

    // 10. While totalBytesToCopyRemaining > 0,
    while (total_bytes_to_copy_remaining > 0) {
        // 1. Let headOfQueue be queue[0].
        auto& head_of_queue = queue.first();

        // 2. Let bytesToCopy be min(totalBytesToCopyRemaining, headOfQueue’s byte length).
        auto bytes_to_copy = min(total_bytes_to_copy_remaining, head_of_queue.byte_length);

        // 3. Let destStart be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s bytes filled.
        auto dest_start = pull_into_descriptor.byte_offset + pull_into_descriptor.bytes_filled;

        // 4. Perform ! CopyDataBlockBytes(pullIntoDescriptor’s buffer.[[ArrayBufferData]], destStart, headOfQueue’s buffer.[[ArrayBufferData]], headOfQueue’s byte offset, bytesToCopy).
        JS::copy_data_block_bytes(pull_into_descriptor.buffer->buffer(), dest_start, head_of_queue.buffer->buffer(), head_of_queue.byte_offset, bytes_to_copy);

        // 5. If headOfQueue’s byte length is bytesToCopy,
        if (head_of_queue.byte_length == bytes_to_copy) {
            // 1. Remove queue[0].
            queue.take_first();
        }
        // 6. Otherwise,
        else {
            // 1. Set headOfQueue’s byte offset to headOfQueue’s byte offset + bytesToCopy.
            head_of_queue.byte_offset += bytes_to_copy;

            // 2. Set headOfQueue’s byte length to headOfQueue’s byte length − bytesToCopy.
            head_of_queue.byte_length -= bytes_to_copy;
        }

        // 7. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] − bytesToCopy.
        controller.set_queue_total_size(controller.queue_total_size() - bytes_to_copy);

        // 8, Perform ! ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller, bytesToCopy, pullIntoDescriptor).
        readable_byte_stream_controller_fill_head_pull_into_descriptor(controller, bytes_to_copy, pull_into_descriptor);

        // 9. Set totalBytesToCopyRemaining to totalBytesToCopyRemaining − bytesToCopy.
        total_bytes_to_copy_remaining -= bytes_to_copy;
    }

    // 11. If ready is false,
    if (!ready) {
        // 1. Assert: controller.[[queueTotalSize]] is 0.
        VERIFY(controller.queue_total_size() == 0);

        // 2. Assert: pullIntoDescriptor’s bytes filled > 0.
        VERIFY(pull_into_descriptor.bytes_filled > 0);

        // 3. Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s element size.
        VERIFY(pull_into_descriptor.bytes_filled < pull_into_descriptor.element_size);
    }

    // 12. Return ready.
    return ready;
}

// https://streams.spec.whatwg.org/#readable-stream-default-reader-read
void readable_stream_default_reader_read(ReadableStreamDefaultReader& reader, ReadRequest& read_request)
{
    // 1. Let stream be reader.[[stream]].
    auto stream = reader.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Set stream.[[disturbed]] to true.
    stream->set_disturbed(true);

    // 4. If stream.[[state]] is "closed", perform readRequest’s close steps.
    if (stream->is_closed()) {
        read_request.on_close();
    }
    // 5. Otherwise, if stream.[[state]] is "errored", perform readRequest’s error steps given stream.[[storedError]].
    else if (stream->is_errored()) {
        read_request.on_error(stream->stored_error());
    }
    // 6. Otherwise,
    else {
        // 1. Assert: stream.[[state]] is "readable".
        VERIFY(stream->is_readable());

        // 2. Perform ! stream.[[controller]].[[PullSteps]](readRequest).
        stream->controller()->visit([&](auto const& controller) {
            return controller->pull_steps(read_request);
        });
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-convert-pull-into-descriptor
JS::Value readable_byte_stream_controller_convert_pull_into_descriptor(JS::Realm& realm, PullIntoDescriptor const& pull_into_descriptor)
{
    auto& vm = realm.vm();

    // 1. Let bytesFilled be pullIntoDescriptor’s bytes filled.
    auto bytes_filled = pull_into_descriptor.bytes_filled;

    // 2. Let elementSize be pullIntoDescriptor’s element size.
    auto element_size = pull_into_descriptor.element_size;

    // 3. Assert: bytesFilled ≤ pullIntoDescriptor’s byte length.
    VERIFY(bytes_filled <= pull_into_descriptor.byte_length);

    // 4. Assert: bytesFilled mod elementSize is 0.
    VERIFY(bytes_filled % element_size == 0);

    // 5. Let buffer be ! TransferArrayBuffer(pullIntoDescriptor’s buffer).
    auto buffer = MUST(transfer_array_buffer(realm, pull_into_descriptor.buffer));

    // 6. Return ! Construct(pullIntoDescriptor’s view constructor, « buffer, pullIntoDescriptor’s byte offset, bytesFilled ÷ elementSize »).
    return MUST(JS::construct(vm, *pull_into_descriptor.view_constructor, buffer, JS::Value(pull_into_descriptor.byte_offset), JS::Value(bytes_filled / element_size)));
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-pull-into
void readable_byte_stream_controller_pull_into(ReadableByteStreamController& controller, WebIDL::ArrayBufferView& view, ReadIntoRequest& read_into_request)
{
    auto& vm = controller.vm();
    auto& realm = controller.realm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Let elementSize be 1.
    size_t element_size = 1;

    // 3. Let ctor be %DataView%.
    JS::NativeFunction* ctor = realm.intrinsics().data_view_constructor();

    // 4. If view has a [[TypedArrayName]] internal slot (i.e., it is not a DataView),
    if (view.bufferable_object().has<JS::NonnullGCPtr<JS::TypedArrayBase>>()) {
        auto const& typed_array = *view.bufferable_object().get<JS::NonnullGCPtr<JS::TypedArrayBase>>();

        // 1. Set elementSize to the element size specified in the typed array constructors table for view.[[TypedArrayName]].
        element_size = typed_array.element_size();

        // 2. Set ctor to the constructor specified in the typed array constructors table for view.[[TypedArrayName]].
        switch (typed_array.kind()) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case JS::TypedArrayBase::Kind::ClassName:                                       \
        ctor = realm.intrinsics().snake_name##_constructor();                       \
        break;
            JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
        }
    }

    // 5. Let byteOffset be view.[[ByteOffset]].
    auto byte_offset = view.byte_offset();

    // 6. Let byteLength be view.[[ByteLength]].
    auto byte_length = view.byte_length();

    // 7. Let bufferResult be TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
    auto buffer_result = transfer_array_buffer(realm, *view.viewed_array_buffer());

    // 8. If bufferResult is an abrupt completion,
    if (buffer_result.is_exception()) {
        // 1. Perform readIntoRequest’s error steps, given bufferResult.[[Value]].
        auto throw_completion = Bindings::dom_exception_to_throw_completion(vm, buffer_result.exception());
        read_into_request.on_error(*throw_completion.release_value());

        // 2. Return.
        return;
    }

    // 9. Let buffer be bufferResult.[[Value]].
    auto buffer = buffer_result.value();

    // 10. Let pullIntoDescriptor be a new pull-into descriptor with buffer buffer, buffer byte length buffer.[[ArrayBufferByteLength]],
    //     byte offset byteOffset, byte length byteLength, bytes filled 0, element size elementSize, view constructor ctor, and reader type "byob".
    PullIntoDescriptor pull_into_descriptor {
        .buffer = buffer,
        .buffer_byte_length = buffer->byte_length(),
        .byte_offset = byte_offset,
        .byte_length = byte_length,
        .bytes_filled = 0,
        .element_size = element_size,
        .view_constructor = *ctor,
        .reader_type = ReaderType::Byob,
    };

    // 11. If controller.[[pendingPullIntos]] is not empty,
    if (!controller.pending_pull_intos().is_empty()) {
        // 1. Append pullIntoDescriptor to controller.[[pendingPullIntos]].
        controller.pending_pull_intos().append(pull_into_descriptor);

        // 2. Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
        readable_stream_add_read_into_request(*stream, read_into_request);

        // 3. Return.
        return;
    }

    // 12. If stream.[[state]] is "closed",
    if (stream->is_closed()) {
        // 1. Let emptyView be ! Construct(ctor, « pullIntoDescriptor’s buffer, pullIntoDescriptor’s byte offset, 0 »).
        auto empty_view = MUST(JS::construct(vm, *ctor, pull_into_descriptor.buffer, JS::Value(pull_into_descriptor.byte_offset), JS::Value(0)));

        // 2. Perform readIntoRequest’s close steps, given emptyView.
        read_into_request.on_close(empty_view);

        // 3. Return.
        return;
    }

    // 13. If controller.[[queueTotalSize]] > 0,
    if (controller.queue_total_size() > 0) {
        // 1. If ! ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, pullIntoDescriptor) is true,
        if (readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(controller, pull_into_descriptor)) {
            // 1. Let filledView be ! ReadableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor).
            auto filled_view = readable_byte_stream_controller_convert_pull_into_descriptor(realm, pull_into_descriptor);

            // 2. Perform ! ReadableByteStreamControllerHandleQueueDrain(controller).
            readable_byte_stream_controller_handle_queue_drain(controller);

            // 3. Perform readIntoRequest’s chunk steps, given filledView.
            read_into_request.on_chunk(filled_view);

            // 4. Return.
            return;
        }

        // 2. If controller.[[closeRequested]] is true,
        if (controller.close_requested()) {
            // 1. Let e be a TypeError exception.
            auto error = JS::TypeError::create(realm, "Reader has been released"sv);

            // 2. Perform ! ReadableByteStreamControllerError(controller, e).
            readable_byte_stream_controller_error(controller, error);

            // 3. Perform readIntoRequest’s error steps, given e.
            read_into_request.on_error(error);

            // 4. Return.
            return;
        }
    }

    // 14. Append pullIntoDescriptor to controller.[[pendingPullIntos]].
    controller.pending_pull_intos().append(pull_into_descriptor);

    // 15. Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
    readable_stream_add_read_into_request(*stream, read_into_request);

    // 16. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
    readable_byte_stream_controller_call_pull_if_needed(controller);
}

// https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
void readable_stream_byob_reader_read(ReadableStreamBYOBReader& reader, WebIDL::ArrayBufferView& view, ReadIntoRequest& read_into_request)
{
    // 1. Let stream be reader.[[stream]].
    auto stream = reader.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Set stream.[[disturbed]] to true.
    stream->set_disturbed(true);

    // 4. If stream.[[state]] is "errored", perform readIntoRequest’s error steps given stream.[[storedError]].
    if (stream->is_errored()) {
        read_into_request.on_error(stream->stored_error());
    }
    // 5. Otherwise, perform ! ReadableByteStreamControllerPullInto(stream.[[controller]], view, readIntoRequest).
    else {
        readable_byte_stream_controller_pull_into(*stream->controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>(), view, read_into_request);
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreaderrelease
void readable_stream_default_reader_release(ReadableStreamDefaultReader& reader)
{
    auto& realm = reader.realm();

    // 1. Perform ! ReadableStreamReaderGenericRelease(reader).
    readable_stream_reader_generic_release(reader);

    // 2. Let e be a new TypeError exception.
    auto exception = JS::TypeError::create(realm, "Reader has been released"sv);

    // 3. Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e).
    readable_stream_default_reader_error_read_requests(reader, exception);
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreaderrelease
void readable_stream_byob_reader_release(ReadableStreamBYOBReader& reader)
{
    auto& realm = reader.realm();

    // 1. Perform ! ReadableStreamReaderGenericRelease(reader).
    readable_stream_reader_generic_release(reader);

    // 2. Let e be a new TypeError exception.
    auto exception = JS::TypeError::create(realm, "Reader has been released"sv);

    // 3. Perform ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
    readable_stream_byob_reader_error_read_into_requests(reader, exception);
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-reader
WebIDL::ExceptionOr<void> set_up_readable_stream_default_reader(ReadableStreamDefaultReader& reader, ReadableStream& stream)
{
    // 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError exception.
    if (is_readable_stream_locked(stream))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create stream reader for a locked stream"sv };

    // 2. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
    // 3. Set reader.[[readRequests]] to a new empty list.
    readable_stream_reader_generic_initialize(ReadableStreamReader { reader }, stream);

    return {};
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
WebIDL::ExceptionOr<void> set_up_readable_stream_byob_reader(ReadableStreamBYOBReader& reader, ReadableStream& stream)
{
    // 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError exception.
    if (is_readable_stream_locked(stream))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create stream reader for a locked stream"sv };

    // 2. If stream.[[controller]] does not implement ReadableByteStreamController, throw a TypeError exception.
    if (!stream.controller()->has<JS::NonnullGCPtr<ReadableByteStreamController>>())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "BYOB reader cannot set up reader from non-byte stream"sv };

    // 3. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
    readable_stream_reader_generic_initialize(ReadableStreamReader { reader }, stream);

    // 4. Set reader.[[readIntoRequests]] to a new empty list.
    reader.read_into_requests().clear();

    return {};
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-close
void readable_stream_default_controller_close(ReadableStreamDefaultController& controller)
{
    // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller) is false, return.
    if (!readable_stream_default_controller_can_close_or_enqueue(controller))
        return;

    // 2. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 3. Set controller.[[closeRequested]] to true.
    controller.set_close_requested(true);

    // 4. If controller.[[queue]] is empty,
    if (controller.queue().is_empty()) {
        // 1. Perform ! ReadableStreamDefaultControllerClearAlgorithms(controller).
        readable_stream_default_controller_clear_algorithms(controller);

        // 2. Perform ! ReadableStreamClose(stream).
        readable_stream_close(*stream);
    }
}

// https://streams.spec.whatwg.org/#rs-default-controller-has-backpressure
bool readable_stream_default_controller_has_backpressure(ReadableStreamDefaultController& controller)
{
    // 1. If ! ReadableStreamDefaultControllerShouldCallPull(controller) is true, return false.
    if (readable_stream_default_controller_should_call_pull(controller))
        return false;

    // 2. Otherwise, return true.
    return true;
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-enqueue
WebIDL::ExceptionOr<void> readable_stream_default_controller_enqueue(ReadableStreamDefaultController& controller, JS::Value chunk)
{
    auto& vm = controller.vm();

    // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller) is false, return.
    if (!readable_stream_default_controller_can_close_or_enqueue(controller))
        return {};

    // 2. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 3. If ! IsReadableStreamLocked(stream) is true and ! ReadableStreamGetNumReadRequests(stream) > 0, perform ! ReadableStreamFulfillReadRequest(stream, chunk, false).
    if (is_readable_stream_locked(*stream) && readable_stream_get_num_read_requests(*stream) > 0) {
        readable_stream_fulfill_read_request(*stream, chunk, false);
    }
    // 4. Otherwise,
    else {
        // 1. Let result be the result of performing controller.[[strategySizeAlgorithm]], passing in chunk, and interpreting the result as a completion record.
        auto result = controller.strategy_size_algorithm()->function()(chunk);

        // 2. If result is an abrupt completion,
        if (result.is_abrupt()) {
            // 1. Perform ! ReadableStreamDefaultControllerError(controller, result.[[Value]]).
            readable_stream_default_controller_error(controller, result.value().value());

            // 2. Return result.
            return result;
        }

        // 3. Let chunkSize be result.[[Value]].
        auto chunk_size = result.release_value().release_value();

        // 4. Let enqueueResult be EnqueueValueWithSize(controller, chunk, chunkSize).
        auto enqueue_result = enqueue_value_with_size(controller, chunk, chunk_size);

        // 5. If enqueueResult is an abrupt completion,
        if (enqueue_result.is_error()) {
            auto throw_completion = Bindings::throw_dom_exception_if_needed(vm, [&] { return enqueue_result; }).throw_completion();

            // 1. Perform ! ReadableStreamDefaultControllerError(controller, enqueueResult.[[Value]]).
            readable_stream_default_controller_error(controller, throw_completion.value().value());

            // 2. Return enqueueResult.
            // Note: We need to return the throw_completion object here, as enqueue needs to throw the same object that the controller is errored with
            return throw_completion;
        }
    }

    // 5. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
    readable_stream_default_controller_can_pull_if_needed(controller);
    return {};
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-call-pull-if-needed
void readable_stream_default_controller_can_pull_if_needed(ReadableStreamDefaultController& controller)
{
    // 1. Let shouldPull be ! ReadableStreamDefaultControllerShouldCallPull(controller).
    auto should_pull = readable_stream_default_controller_should_call_pull(controller);

    // 2. If shouldPull is false, return.
    if (!should_pull)
        return;

    // 3. If controller.[[pulling]] is true,
    if (controller.pulling()) {
        // 1. Set controller.[[pullAgain]] to true.
        controller.set_pull_again(true);

        // 2. Return.
        return;
    }

    // 4. Assert: controller.[[pullAgain]] is false.
    VERIFY(!controller.pull_again());

    // 5. Set controller.[[pulling]] to true.
    controller.set_pulling(true);

    // 6. Let pullPromise be the result of performing controller.[[pullAlgorithm]].
    auto pull_promise = controller.pull_algorithm()->function()();

    // 7. Upon fulfillment of pullPromise,
    WebIDL::upon_fulfillment(*pull_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Set controller.[[pulling]] to false.
        controller.set_pulling(false);

        // 2. If controller.[[pullAgain]] is true,
        if (controller.pull_again()) {
            // 1. Set controller.[[pullAgain]] to false.
            controller.set_pull_again(false);

            // 2. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
            readable_stream_default_controller_can_pull_if_needed(controller);
        }

        return JS::js_undefined();
    }));

    // 8. Upon rejection of pullPromise with reason e,
    WebIDL::upon_rejection(*pull_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value e) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform ! ReadableStreamDefaultControllerError(controller, e).
        readable_stream_default_controller_error(controller, e);

        return JS::js_undefined();
    }));
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-should-call-pull
bool readable_stream_default_controller_should_call_pull(ReadableStreamDefaultController& controller)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller) is false, return false.
    if (!readable_stream_default_controller_can_close_or_enqueue(controller))
        return false;

    // 3. If controller.[[started]] is false, return false.
    if (!controller.started())
        return false;

    // 4. If ! IsReadableStreamLocked(stream) is true and ! ReadableStreamGetNumReadRequests(stream) > 0, return true.
    if (is_readable_stream_locked(*stream) && readable_stream_get_num_read_requests(*stream) > 0)
        return true;

    // 5. Let desiredSize be ! ReadableStreamDefaultControllerGetDesiredSize(controller).
    auto desired_size = readable_stream_default_controller_get_desired_size(controller);

    // 6. Assert: desiredSize is not null.
    VERIFY(desired_size.has_value());

    // 7. If desiredSize > 0, return true.
    if (desired_size.release_value() > 0.0)
        return true;

    // 8. Return false.
    return false;
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollergetbyobrequest
JS::GCPtr<ReadableStreamBYOBRequest> readable_byte_stream_controller_get_byob_request(JS::NonnullGCPtr<ReadableByteStreamController> controller)
{
    auto& vm = controller->vm();
    auto& realm = controller->realm();

    // 1. If controller.[[byobRequest]] is null and controller.[[pendingPullIntos]] is not empty,
    if (!controller->raw_byob_request() && !controller->pending_pull_intos().is_empty()) {
        // 1. Let firstDescriptor be controller.[[pendingPullIntos]][0].
        auto const& first_descriptor = controller->pending_pull_intos().first();

        // 2. Let view be ! Construct(%Uint8Array%, « firstDescriptor’s buffer, firstDescriptor’s byte offset + firstDescriptor’s bytes filled, firstDescriptor’s byte length − firstDescriptor’s bytes filled »).
        auto view = MUST(JS::construct(vm, *realm.intrinsics().uint8_array_constructor(), first_descriptor.buffer, JS::Value(first_descriptor.byte_offset + first_descriptor.bytes_filled), JS::Value(first_descriptor.byte_length - first_descriptor.bytes_filled)));

        // 3. Let byobRequest be a new ReadableStreamBYOBRequest.
        auto byob_request = realm.heap().allocate<ReadableStreamBYOBRequest>(realm, realm);

        // 4. Set byobRequest.[[controller]] to controller.
        byob_request->set_controller(controller);

        // 5. Set byobRequest.[[view]] to view.
        auto array_buffer_view = vm.heap().allocate<WebIDL::ArrayBufferView>(realm, view);
        byob_request->set_view(array_buffer_view);

        // 6. Set controller.[[byobRequest]] to byobRequest.
        controller->set_byob_request(byob_request);
    }

    // 2. Return controller.[[byobRequest]].
    return controller->raw_byob_request();
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-clear-algorithms
void readable_stream_default_controller_clear_algorithms(ReadableStreamDefaultController& controller)
{
    // 1. Set controller.[[pullAlgorithm]] to undefined.
    controller.set_pull_algorithm({});

    // 2. Set controller.[[cancelAlgorithm]] to undefined.
    controller.set_cancel_algorithm({});

    // 3. Set controller.[[strategySizeAlgorithm]] to undefined.
    controller.set_strategy_size_algorithm({});
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-readable-state
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_in_readable_state(ReadableByteStreamController& controller, u64 bytes_written, PullIntoDescriptor& pull_into_descriptor)
{
    // 1. Assert: pullIntoDescriptor’s bytes filled + bytesWritten ≤ pullIntoDescriptor’s byte length.
    VERIFY(pull_into_descriptor.bytes_filled + bytes_written <= pull_into_descriptor.byte_length);

    // 2. Perform ! ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller, bytesWritten, pullIntoDescriptor).
    readable_byte_stream_controller_fill_head_pull_into_descriptor(controller, bytes_written, pull_into_descriptor);

    // 3. If pullIntoDescriptor’s reader type is "none",
    if (pull_into_descriptor.reader_type == ReaderType::None) {
        // 1. Perform ? ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, pullIntoDescriptor).
        TRY(readable_byte_stream_controller_enqueue_detached_pull_into_queue(controller, pull_into_descriptor));

        // 2. Perform ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
        readable_byte_stream_controller_process_pull_into_descriptors_using_queue(controller);

        // 3. Return.
        return {};
    }

    // 4. If pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s minimum fill, return.
    // FIXME: Support minimum fill.
    if (pull_into_descriptor.bytes_filled < pull_into_descriptor.element_size)
        return {};

    // NOTE: A descriptor for a read() request that is not yet filled up to its minimum length will stay at the head of the queue, so the underlying source can keep filling it.

    // 5. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
    // NOTE: We need to take a copy of pull_into_descriptor here as the shift destroys the pull into descriptor we are given.
    auto pull_into_descriptor_copy = readable_byte_stream_controller_shift_pending_pull_into(controller);

    // 6. Let remainderSize be the remainder after dividing pullIntoDescriptor’s bytes filled by pullIntoDescriptor’s element size.
    auto remainder_size = pull_into_descriptor_copy.bytes_filled % pull_into_descriptor_copy.element_size;

    // 7. If remainderSize > 0,
    if (remainder_size > 0) {
        // 1. Let end be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s bytes filled.
        auto end = pull_into_descriptor_copy.byte_offset + pull_into_descriptor_copy.bytes_filled;

        // 2. Perform ? ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller, pullIntoDescriptor’s buffer, end − remainderSize, remainderSize).
        TRY(readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(controller, *pull_into_descriptor_copy.buffer, end - remainder_size, remainder_size));
    }

    // 8. Set pullIntoDescriptor’s bytes filled to pullIntoDescriptor’s bytes filled − remainderSize.
    pull_into_descriptor_copy.bytes_filled -= remainder_size;

    // 9. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], pullIntoDescriptor).
    readable_byte_stream_controller_commit_pull_into_descriptor(*controller.stream(), pull_into_descriptor_copy);
    return {};
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-closed-state
void readable_byte_stream_controller_respond_in_closed_state(ReadableByteStreamController& controller, PullIntoDescriptor& first_descriptor)
{
    // 1. Assert: the remainder after dividing firstDescriptor’s bytes filled by firstDescriptor’s element size is 0.
    VERIFY(first_descriptor.bytes_filled % first_descriptor.element_size == 0);

    // 2. If firstDescriptor’s reader type is "none", perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
    if (first_descriptor.reader_type == ReaderType::None)
        readable_byte_stream_controller_shift_pending_pull_into(controller);

    // 3. Let stream be controller.[[stream]].
    auto& stream = *controller.stream();

    // 4. If ! ReadableStreamHasBYOBReader(stream) is true,
    if (readable_stream_has_byob_reader(stream)) {
        // 1. While ! ReadableStreamGetNumReadIntoRequests(stream) > 0,
        while (readable_stream_get_num_read_into_requests(stream) > 0) {
            // 1. Let pullIntoDescriptor be ! ReadableByteStreamControllerShiftPendingPullInto(controller).
            auto pull_into_descriptor = readable_byte_stream_controller_shift_pending_pull_into(controller);

            // 2. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(stream, pullIntoDescriptor).
            readable_byte_stream_controller_commit_pull_into_descriptor(stream, pull_into_descriptor);
        }
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-internal
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_internal(ReadableByteStreamController& controller, u64 bytes_written)
{
    // 1. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    auto& first_descriptor = controller.pending_pull_intos().first();

    // 2. Assert: ! CanTransferArrayBuffer(firstDescriptor’s buffer) is true.
    VERIFY(can_transfer_array_buffer(*first_descriptor.buffer));

    // 3. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
    readable_byte_stream_controller_invalidate_byob_request(controller);

    // 4. Let state be controller.[[stream]].[[state]].
    auto state = controller.stream()->state();

    // 5. If state is "closed",
    if (state == ReadableStream::State::Closed) {
        // 1. Assert: bytesWritten is 0.
        VERIFY(bytes_written == 0);

        // 2. Perform ! ReadableByteStreamControllerRespondInClosedState(controller, firstDescriptor).
        readable_byte_stream_controller_respond_in_closed_state(controller, first_descriptor);
    }

    // 6. Otherwise,
    else {
        // 1. Assert: state is "readable".
        VERIFY(state == ReadableStream::State::Readable);

        // 2. Assert: bytesWritten > 0.
        VERIFY(bytes_written > 0);

        // 3. Perform ? ReadableByteStreamControllerRespondInReadableState(controller, bytesWritten, firstDescriptor).
        TRY(readable_byte_stream_controller_respond_in_readable_state(controller, bytes_written, first_descriptor));
    }

    // 7. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
    readable_byte_stream_controller_call_pull_if_needed(controller);
    return {};
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond(ReadableByteStreamController& controller, u64 bytes_written)
{
    auto& realm = controller.realm();

    // 1. Assert: controller.[[pendingPullIntos]] is not empty.
    VERIFY(!controller.pending_pull_intos().is_empty());

    // 2. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    auto& first_descriptor = controller.pending_pull_intos().first();

    // 3. Let state be controller.[[stream]].[[state]].
    auto state = controller.stream()->state();

    // 4. If state is "closed",
    if (state == ReadableStream::State::Closed) {
        // 1. If bytesWritten is not 0, throw a TypeError exception.
        if (bytes_written != 0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Bytes written is not zero for closed stream"sv };
    }
    // 5. Otherwise,
    else {
        // 1. Assert: state is "readable".
        VERIFY(state == ReadableStream::State::Readable);

        // 2. If bytesWritten is 0, throw a TypeError exception.
        if (bytes_written == 0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Bytes written is zero for stream which is not closed"sv };

        // 3. If firstDescriptor’s bytes filled + bytesWritten > firstDescriptor’s byte length, throw a RangeError exception.
        if (first_descriptor.bytes_filled + bytes_written > first_descriptor.byte_length)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Bytes written is greater than the pull requests byte length"sv };
    }

    // 6. Set firstDescriptor’s buffer to ! TransferArrayBuffer(firstDescriptor’s buffer).
    first_descriptor.buffer = MUST(transfer_array_buffer(realm, *first_descriptor.buffer));

    // 7. Perform ? ReadableByteStreamControllerRespondInternal(controller, bytesWritten).
    return readable_byte_stream_controller_respond_internal(controller, bytes_written);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-with-new-view
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_with_new_view(JS::Realm& realm, ReadableByteStreamController& controller, WebIDL::ArrayBufferView& view)
{
    // 1. Assert: controller.[[pendingPullIntos]] is not empty.
    VERIFY(!controller.pending_pull_intos().is_empty());

    // 2. Assert: ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is false.
    VERIFY(!view.viewed_array_buffer()->is_detached());

    // 3. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    auto& first_descriptor = controller.pending_pull_intos().first();

    // 4. Let state be controller.[[stream]].[[state]].
    auto state = controller.stream()->state();

    // 5. If state is "closed",
    if (state == ReadableStream::State::Closed) {
        // 1. If view.[[ByteLength]] is not 0, throw a TypeError exception.
        if (view.byte_length() != 0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Byte length is not zero for closed stream"sv };
    }
    // 6. Otherwise,
    else {
        // 1. Assert: state is "readable".
        VERIFY(state == ReadableStream::State::Readable);

        // 2. If view.[[ByteLength]] is 0, throw a TypeError exception.
        if (view.byte_length() == 0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Byte length is zero for stream which is not closed"sv };
    }

    // 7. If firstDescriptor’s byte offset + firstDescriptor’ bytes filled is not view.[[ByteOffset]], throw a RangeError exception.
    if (first_descriptor.byte_offset + first_descriptor.bytes_filled != view.byte_offset())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Byte offset is not aligned with the pull request's byte offset"sv };

    // 8. If firstDescriptor’s buffer byte length is not view.[[ViewedArrayBuffer]].[[ByteLength]], throw a RangeError exception.
    if (first_descriptor.buffer_byte_length != view.viewed_array_buffer()->byte_length())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Buffer byte length is not aligned with the pull request's byte length"sv };

    // 9. If firstDescriptor’s bytes filled + view.[[ByteLength]] > firstDescriptor’s byte length, throw a RangeError exception.
    if (first_descriptor.bytes_filled + view.byte_length() > first_descriptor.byte_length)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Byte length is greater than the pull request's byte length"sv };

    // 10. Let viewByteLength be view.[[ByteLength]].
    auto view_byte_length = view.byte_length();

    // 11. Set firstDescriptor’s buffer to ? TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
    first_descriptor.buffer = TRY(transfer_array_buffer(realm, *view.viewed_array_buffer()));

    // 12. Perform ? ReadableByteStreamControllerRespondInternal(controller, viewByteLength).
    TRY(readable_byte_stream_controller_respond_internal(controller, view_byte_length));

    return {};
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-error
void readable_stream_default_controller_error(ReadableStreamDefaultController& controller, JS::Value error)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If stream.[[state]] is not "readable", return.
    if (!stream->is_readable())
        return;

    // 3. Perform ! ResetQueue(controller).
    reset_queue(controller);

    // 4. Perform ! ReadableStreamDefaultControllerClearAlgorithms(controller).
    readable_stream_default_controller_clear_algorithms(controller);

    // 5. Perform ! ReadableStreamError(stream, e).
    readable_stream_error(*stream, error);
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-get-desired-size
Optional<double> readable_stream_default_controller_get_desired_size(ReadableStreamDefaultController& controller)
{
    auto stream = controller.stream();

    // 1. Let state be controller.[[stream]].[[state]].

    // 2. If state is "errored", return null.
    if (stream->is_errored())
        return {};

    // 3. If state is "closed", return 0.
    if (stream->is_closed())
        return 0.0;

    // 4. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
    return controller.strategy_hwm() - controller.queue_total_size();
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-can-close-or-enqueue
bool readable_stream_default_controller_can_close_or_enqueue(ReadableStreamDefaultController& controller)
{
    // 1. Let state be controller.[[stream]].[[state]].
    // 2. If controller.[[closeRequested]] is false and state is "readable", return true.
    // 3. Otherwise, return false.
    return !controller.close_requested() && controller.stream()->is_readable();
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller(ReadableStream& stream, ReadableStreamDefaultController& controller, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[controller]] is undefined.
    VERIFY(!stream.controller().has_value());

    // 2. Set controller.[[stream]] to stream.
    controller.set_stream(stream);

    // 3. Perform ! ResetQueue(controller).
    reset_queue(controller);

    // 4. Set controller.[[started]], controller.[[closeRequested]], controller.[[pullAgain]], and controller.[[pulling]] to false.
    controller.set_started(false);
    controller.set_close_requested(false);
    controller.set_pull_again(false);
    controller.set_pulling(false);

    // 5. Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm and controller.[[strategyHWM]] to highWaterMark.
    controller.set_strategy_size_algorithm(size_algorithm);
    controller.set_strategy_hwm(high_water_mark);

    // 6. Set controller.[[pullAlgorithm]] to pullAlgorithm.
    controller.set_pull_algorithm(pull_algorithm);

    // 7. Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
    controller.set_cancel_algorithm(cancel_algorithm);

    // 8. Set stream.[[controller]] to controller.
    stream.set_controller(ReadableStreamController { controller });

    // 9. Let startResult be the result of performing startAlgorithm. (This might throw an exception.)
    auto start_result = TRY(start_algorithm->function()());

    // 10. Let startPromise be a promise resolved with startResult.
    auto start_promise = WebIDL::create_resolved_promise(realm, start_result);

    // 11. Upon fulfillment of startPromise,
    WebIDL::upon_fulfillment(start_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Set controller.[[started]] to true.
        controller.set_started(true);

        // 2. Assert: controller.[[pulling]] is false.
        VERIFY(!controller.pulling());

        // 3. Assert: controller.[[pullAgain]] is false.
        VERIFY(!controller.pull_again());

        // 4. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
        readable_stream_default_controller_can_pull_if_needed(controller);

        return JS::js_undefined();
    }));

    // 12. Upon rejection of startPromise with reason r,
    WebIDL::upon_rejection(start_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value r) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform ! ReadableStreamDefaultControllerError(controller, r).
        readable_stream_default_controller_error(controller, r);

        return JS::js_undefined();
    }));

    return {};
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller-from-underlying-source
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller_from_underlying_source(ReadableStream& stream, JS::Value underlying_source_value, UnderlyingSource underlying_source, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Let controller be a new ReadableStreamDefaultController.
    auto controller = stream.heap().allocate<ReadableStreamDefaultController>(realm, realm);

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = JS::create_heap_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 3. Let pullAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto pull_algorithm = JS::create_heap_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let cancelAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto cancel_algorithm = JS::create_heap_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. If underlyingSourceDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of invoking underlyingSourceDict["start"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source.start) {
        start_algorithm = JS::create_heap_function(realm.heap(), [controller, underlying_source_value, callback = underlying_source.start]() -> WebIDL::ExceptionOr<JS::Value> {
            // Note: callback does not return a promise, so invoke_callback may return an abrupt completion
            return TRY(WebIDL::invoke_callback(*callback, underlying_source_value, controller)).release_value();
        });
    }

    // 6. If underlyingSourceDict["pull"] exists, then set pullAlgorithm to an algorithm which returns the result of invoking underlyingSourceDict["pull"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source.pull) {
        pull_algorithm = JS::create_heap_function(realm.heap(), [&realm, controller, underlying_source_value, callback = underlying_source.pull]() {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, underlying_source_value, controller)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 7. If underlyingSourceDict["cancel"] exists, then set cancelAlgorithm to an algorithm which takes an argument reason and returns the result of invoking underlyingSourceDict["cancel"] with argument list « reason » and callback this value underlyingSource.
    if (underlying_source.cancel) {
        cancel_algorithm = JS::create_heap_function(realm.heap(), [&realm, underlying_source_value, callback = underlying_source.cancel](JS::Value reason) {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, underlying_source_value, reason)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 8. Perform ? SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm).
    return set_up_readable_stream_default_controller(stream, controller, start_algorithm, pull_algorithm, cancel_algorithm, high_water_mark, size_algorithm);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-call-pull-if-needed
void readable_byte_stream_controller_call_pull_if_needed(ReadableByteStreamController& controller)
{
    // 1. Let shouldPull be ! ReadableByteStreamControllerShouldCallPull(controller).
    auto should_pull = readable_byte_stream_controller_should_call_pull(controller);

    // 2. If shouldPull is false, return.
    if (!should_pull)
        return;

    // 3. If controller.[[pulling]] is true,
    if (controller.pulling()) {
        // 1. Set controller.[[pullAgain]] to true.
        controller.set_pull_again(true);

        // 2. Return.
        return;
    }

    // 4. Assert: controller.[[pullAgain]] is false.
    VERIFY(!controller.pull_again());

    // 5. Set controller.[[pulling]] to true.
    controller.set_pulling(true);

    // 6. Let pullPromise be the result of performing controller.[[pullAlgorithm]].
    auto pull_promise = controller.pull_algorithm()->function()();

    // 7. Upon fulfillment of pullPromise,
    WebIDL::upon_fulfillment(*pull_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Set controller.[[pulling]] to false.
        controller.set_pulling(false);

        // 2. If controller.[[pullAgain]] is true,
        if (controller.pull_again()) {
            // 1. Set controller.[[pullAgain]] to false.
            controller.set_pull_again(false);

            // 2. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
            readable_byte_stream_controller_call_pull_if_needed(controller);
        }

        return JS::js_undefined();
    }));

    // 8. Upon rejection of pullPromise with reason e,
    WebIDL::upon_rejection(*pull_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value error) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform ! ReadableByteStreamControllerError(controller, e).
        readable_byte_stream_controller_error(controller, error);

        return JS::js_undefined();
    }));
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-algorithms
void readable_byte_stream_controller_clear_algorithms(ReadableByteStreamController& controller)
{
    // 1. Set controller.[[pullAlgorithm]] to undefined.
    controller.set_pull_algorithm({});

    // 2. Set controller.[[cancelAlgorithm]] to undefined.
    controller.set_cancel_algorithm({});
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-pending-pull-intos
void readable_byte_stream_controller_clear_pending_pull_intos(ReadableByteStreamController& controller)
{
    // 1. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
    readable_byte_stream_controller_invalidate_byob_request(controller);

    // 2. Set controller.[[pendingPullIntos]] to a new empty list.
    controller.pending_pull_intos().clear();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-close
WebIDL::ExceptionOr<void> readable_byte_stream_controller_close(ReadableByteStreamController& controller)
{
    auto& realm = controller.realm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If controller.[[closeRequested]] is true or stream.[[state]] is not "readable", return.
    if (controller.close_requested() || stream->state() != ReadableStream::State::Readable)
        return {};

    // 3. If controller.[[queueTotalSize]] > 0,
    if (controller.queue_total_size() > 0.0) {
        // 1. Set controller.[[closeRequested]] to true.
        controller.set_close_requested(true);

        // 2. Return.
        return {};
    }

    // 4. If controller.[[pendingPullIntos]] is not empty,
    if (!controller.pending_pull_intos().is_empty()) {
        // 1. Let firstPendingPullInto be controller.[[pendingPullIntos]][0].
        auto& first_pending_pull_into = controller.pending_pull_intos().first();

        // 2. If firstPendingPullInto’s bytes filled > 0,
        if (first_pending_pull_into.bytes_filled > 0) {
            // 1. Let e be a new TypeError exception.
            auto error = JS::TypeError::create(realm, "Cannot close controller in the middle of processing a write request"sv);

            // 2. Perform ! ReadableByteStreamControllerError(controller, e).
            readable_byte_stream_controller_error(controller, error);

            // 3. Throw e.
            return JS::throw_completion(error);
        }
    }

    // 5. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
    readable_byte_stream_controller_clear_algorithms(controller);

    // 6. Perform ! ReadableStreamClose(stream).
    readable_stream_close(*stream);

    return {};
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-error
void readable_byte_stream_controller_error(ReadableByteStreamController& controller, JS::Value error)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If stream.[[state]] is not "readable", return.
    if (stream->state() != ReadableStream::State::Readable)
        return;

    // 3. Perform ! ReadableByteStreamControllerClearPendingPullIntos(controller).
    readable_byte_stream_controller_clear_pending_pull_intos(controller);

    // 4. Perform ! ResetQueue(controller).
    reset_queue(controller);

    // 5. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
    readable_byte_stream_controller_clear_algorithms(controller);

    // 6. Perform ! ReadableStreamError(stream, e).
    readable_stream_error(*stream, error);
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerfillreadrequestfromqueue
void readable_byte_stream_controller_fill_read_request_from_queue(ReadableByteStreamController& controller, JS::NonnullGCPtr<ReadRequest> read_request)
{
    auto& vm = controller.vm();
    auto& realm = controller.realm();

    // 1. Assert: controller.[[queueTotalSize]] > 0.
    VERIFY(controller.queue_total_size() > 0.0);

    // 2. Let entry be controller.[[queue]][0].
    // 3. Remove entry from controller.[[queue]].
    auto entry = controller.queue().take_first();

    // 4. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] − entry’s byte length.
    controller.set_queue_total_size(controller.queue_total_size() - entry.byte_length);

    // 5. Perform ! ReadableByteStreamControllerHandleQueueDrain(controller).
    readable_byte_stream_controller_handle_queue_drain(controller);

    // 6. Let view be ! Construct(%Uint8Array%, « entry’s buffer, entry’s byte offset, entry’s byte length »).
    auto view = MUST(JS::construct(vm, *realm.intrinsics().uint8_array_constructor(), entry.buffer, JS::Value(entry.byte_offset), JS::Value(entry.byte_length)));

    // 7. Perform readRequest’s chunk steps, given view.
    read_request->on_chunk(view);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-get-desired-size
Optional<double> readable_byte_stream_controller_get_desired_size(ReadableByteStreamController const& controller)
{
    auto stream = controller.stream();

    // 1. Let state be controller.[[stream]].[[state]].
    // 2. If state is "errored", return null.
    if (stream->is_errored())
        return {};

    // 3. If state is "closed", return 0.
    if (stream->is_closed())
        return 0.0;

    // 4. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
    return controller.strategy_hwm() - controller.queue_total_size();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-handle-queue-drain
void readable_byte_stream_controller_handle_queue_drain(ReadableByteStreamController& controller)
{
    // 1. Assert: controller.[[stream]].[[state]] is "readable".
    VERIFY(controller.stream()->state() == ReadableStream::State::Readable);

    // 2. If controller.[[queueTotalSize]] is 0 and controller.[[closeRequested]] is true,
    if (controller.queue_total_size() == 0.0 && controller.close_requested()) {
        // 1. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
        readable_byte_stream_controller_clear_algorithms(controller);

        // 2. Perform ! ReadableStreamClose(controller.[[stream]]).
        readable_stream_close(*controller.stream());
    }
    // 3. Otherwise,
    else {
        // 1. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
        readable_byte_stream_controller_call_pull_if_needed(controller);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-invalidate-byob-request
void readable_byte_stream_controller_invalidate_byob_request(ReadableByteStreamController& controller)
{
    // 1. If controller.[[byobRequest]] is null, return.
    if (!controller.byob_request())
        return;

    // 2. Set controller.[[byobRequest]].[[controller]] to undefined.
    controller.byob_request()->set_controller({});

    // 3. Set controller.[[byobRequest]].[[view]] to null.
    controller.byob_request()->set_view({});

    // 4. Set controller.[[byobRequest]] to null.
    controller.set_byob_request({});
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-error
bool readable_byte_stream_controller_should_call_pull(ReadableByteStreamController const& controller)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If stream.[[state]] is not "readable", return false.
    if (stream->state() != ReadableStream::State::Readable)
        return false;

    // 3. If controller.[[closeRequested]] is true, return false.
    if (controller.close_requested())
        return false;

    // 4. If controller.[[started]] is false, return false.
    if (!controller.started())
        return false;

    // 5. If ! ReadableStreamHasDefaultReader(stream) is true and ! ReadableStreamGetNumReadRequests(stream) > 0, return true.
    if (readable_stream_has_default_reader(*stream) && readable_stream_get_num_read_requests(*stream) > 0)
        return true;

    // 6. If ! ReadableStreamHasBYOBReader(stream) is true and ! ReadableStreamGetNumReadIntoRequests(stream) > 0, return true.
    if (readable_stream_has_byob_reader(*stream) && readable_stream_get_num_read_into_requests(*stream) > 0)
        return true;

    // 7. Let desiredSize be ! ReadableByteStreamControllerGetDesiredSize(controller).
    auto desired_size = readable_byte_stream_controller_get_desired_size(controller);

    // 8. Assert: desiredSize is not null.
    VERIFY(desired_size.has_value());

    // 9. If desiredSize > 0, return true.
    if (*desired_size > 0.0)
        return true;

    // 10. Return false.
    return false;
}

// NON-STANDARD: Can be used instead of CreateReadableStream in cases where we need to set up a newly allocated
//               ReadableStream before initialization of said ReadableStream, i.e. ReadableStream is captured by lambdas in an uninitialized state.
// Spec steps are taken from: https://streams.spec.whatwg.org/#create-readable-stream
WebIDL::ExceptionOr<void> set_up_readable_stream(JS::Realm& realm, ReadableStream& stream, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm, Optional<double> high_water_mark, JS::GCPtr<SizeAlgorithm> size_algorithm)
{
    // 1. If highWaterMark was not passed, set it to 1.
    if (!high_water_mark.has_value())
        high_water_mark = 1.0;

    // 2. If sizeAlgorithm was not passed, set it to an algorithm that returns 1.
    if (!size_algorithm)
        size_algorithm = JS::create_heap_function(realm.heap(), [](JS::Value) { return JS::normal_completion(JS::Value(1)); });

    // 3. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
    VERIFY(is_non_negative_number(JS::Value { *high_water_mark }));

    // 4. Let stream be a new ReadableStream.
    //    NOTE: The ReadableStream is allocated outside the scope of this method.

    // 5. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(stream);

    // 6. Let controller be a new ReadableStreamDefaultController.
    auto controller = realm.heap().allocate<ReadableStreamDefaultController>(realm, realm);

    // 7. Perform ? SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm).
    TRY(set_up_readable_stream_default_controller(stream, *controller, start_algorithm, pull_algorithm, cancel_algorithm, *high_water_mark, *size_algorithm));

    return {};
}

// https://streams.spec.whatwg.org/#create-readable-stream
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> create_readable_stream(JS::Realm& realm, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm, Optional<double> high_water_mark, JS::GCPtr<SizeAlgorithm> size_algorithm)
{
    // 1. If highWaterMark was not passed, set it to 1.
    if (!high_water_mark.has_value())
        high_water_mark = 1.0;

    // 2. If sizeAlgorithm was not passed, set it to an algorithm that returns 1.
    if (!size_algorithm)
        size_algorithm = JS::create_heap_function(realm.heap(), [](JS::Value) { return JS::normal_completion(JS::Value(1)); });

    // 3. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
    VERIFY(is_non_negative_number(JS::Value { *high_water_mark }));

    // 4. Let stream be a new ReadableStream.
    auto stream = realm.heap().allocate<ReadableStream>(realm, realm);

    // 5. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(*stream);

    // 6. Let controller be a new ReadableStreamDefaultController.
    auto controller = realm.heap().allocate<ReadableStreamDefaultController>(realm, realm);

    // 7. Perform ? SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm).
    TRY(set_up_readable_stream_default_controller(*stream, *controller, start_algorithm, pull_algorithm, cancel_algorithm, *high_water_mark, *size_algorithm));

    // 8. Return stream.
    return stream;
}

// https://streams.spec.whatwg.org/#abstract-opdef-createreadablebytestream
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> create_readable_byte_stream(JS::Realm& realm, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm)
{
    // 1. Let stream be a new ReadableStream.
    auto stream = realm.heap().allocate<ReadableStream>(realm, realm);

    // 2. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(*stream);

    // 3. Let controller be a new ReadableByteStreamController.
    auto controller = realm.heap().allocate<ReadableByteStreamController>(realm, realm);

    // 4. Perform ? SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, 0, undefined).
    TRY(set_up_readable_byte_stream_controller(stream, controller, start_algorithm, pull_algorithm, cancel_algorithm, 0, JS::js_undefined()));

    // 5. Return stream.
    return stream;
}

// https://streams.spec.whatwg.org/#create-writable-stream
WebIDL::ExceptionOr<JS::NonnullGCPtr<WritableStream>> create_writable_stream(JS::Realm& realm, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<WriteAlgorithm> write_algorithm, JS::NonnullGCPtr<CloseAlgorithm> close_algorithm, JS::NonnullGCPtr<AbortAlgorithm> abort_algorithm, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> size_algorithm)
{
    // 1. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
    VERIFY(is_non_negative_number(JS::Value { high_water_mark }));

    // 2. Let stream be a new WritableStream.
    auto stream = realm.heap().allocate<WritableStream>(realm, realm);

    // 3. Perform ! InitializeWritableStream(stream).
    initialize_writable_stream(*stream);

    // 4. Let controller be a new WritableStreamDefaultController.
    auto controller = realm.heap().allocate<WritableStreamDefaultController>(realm, realm);

    // 5. Perform ? SetUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm).
    TRY(set_up_writable_stream_default_controller(*stream, *controller, move(start_algorithm), move(write_algorithm), move(close_algorithm), move(abort_algorithm), high_water_mark, move(size_algorithm)));

    // 6. Return stream.
    return stream;
}

// https://streams.spec.whatwg.org/#initialize-readable-stream
void initialize_readable_stream(ReadableStream& stream)
{
    // 1. Set stream.[[state]] to "readable".
    stream.set_state(ReadableStream::State::Readable);

    // 2. Set stream.[[reader]] and stream.[[storedError]] to undefined.
    stream.set_reader({});
    stream.set_stored_error({});

    // 3. Set stream.[[disturbed]] to false.
    stream.set_disturbed(false);
}

// https://streams.spec.whatwg.org/#initialize-writable-stream
void initialize_writable_stream(WritableStream& stream)
{
    // 1. Set stream.[[state]] to "writable".
    stream.set_state(WritableStream::State::Writable);

    // 2. Set stream.[[storedError]], stream.[[writer]], stream.[[controller]], stream.[[inFlightWriteRequest]],
    //    stream.[[closeRequest]], stream.[[inFlightCloseRequest]], and stream.[[pendingAbortRequest]] to undefined.
    stream.set_stored_error(JS::js_undefined());
    stream.set_writer({});
    stream.set_controller({});
    stream.set_in_flight_write_request({});
    stream.set_close_request({});
    stream.set_in_flight_close_request({});
    stream.set_pending_abort_request({});

    // 3. Set stream.[[writeRequests]] to a new empty list.
    stream.write_requests().clear();

    // 4. Set stream.[[backpressure]] to false.
    stream.set_backpressure(false);
}

// https://streams.spec.whatwg.org/#acquire-writable-stream-default-writer
WebIDL::ExceptionOr<JS::NonnullGCPtr<WritableStreamDefaultWriter>> acquire_writable_stream_default_writer(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let writer be a new WritableStreamDefaultWriter.
    auto writer = stream.heap().allocate<WritableStreamDefaultWriter>(realm, realm);

    // 2. Perform ? SetUpWritableStreamDefaultWriter(writer, stream).
    TRY(set_up_writable_stream_default_writer(*writer, stream));

    // 3. Return writer.
    return writer;
}

// https://streams.spec.whatwg.org/#is-writable-stream-locked
bool is_writable_stream_locked(WritableStream const& stream)
{
    // 1. If stream.[[writer]] is undefined, return false.
    if (!stream.writer())
        return false;

    // 2. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#set-up-writable-stream-default-writer
WebIDL::ExceptionOr<void> set_up_writable_stream_default_writer(WritableStreamDefaultWriter& writer, WritableStream& stream)
{
    auto& realm = writer.realm();

    // 1. If ! IsWritableStreamLocked(stream) is true, throw a TypeError exception.
    if (is_writable_stream_locked(stream))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Stream is locked"sv };

    // 2. Set writer.[[stream]] to stream.
    writer.set_stream(stream);

    // 3. Set stream.[[writer]] to writer.
    stream.set_writer(writer);

    // 4. Let state be stream.[[state]].
    auto state = stream.state();

    // 5. If state is "writable",
    if (state == WritableStream::State::Writable) {
        // 1. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and stream.[[backpressure]] is true, set writer.[[readyPromise]] to a new promise.
        if (!writable_stream_close_queued_or_in_flight(stream) && stream.backpressure()) {
            writer.set_ready_promise(WebIDL::create_promise(realm));
        }
        // 2. Otherwise, set writer.[[readyPromise]] to a promise resolved with undefined.
        else {
            writer.set_ready_promise(WebIDL::create_resolved_promise(realm, JS::js_undefined()));
        }

        // 3. Set writer.[[closedPromise]] to a new promise.
        writer.set_closed_promise(WebIDL::create_promise(realm));
    }
    // 6. Otherwise, if state is "erroring",
    else if (state == WritableStream::State::Erroring) {
        // 1. Set writer.[[readyPromise]] to a promise rejected with stream.[[storedError]].
        writer.set_ready_promise(WebIDL::create_rejected_promise(realm, stream.stored_error()));

        // 2. Set writer.[[readyPromise]].[[PromiseIsHandled]] to true.
        WebIDL::mark_promise_as_handled(*writer.ready_promise());

        // 3. Set writer.[[closedPromise]] to a new promise.
        writer.set_closed_promise(WebIDL::create_promise(realm));
    }
    // 7. Otherwise, if state is "closed",
    else if (state == WritableStream::State::Closed) {
        // 1. Set writer.[[readyPromise]] to a promise resolved with undefined.
        writer.set_ready_promise(WebIDL::create_resolved_promise(realm, JS::js_undefined()));

        // 2. Set writer.[[closedPromise]] to a promise resolved with undefined.
        writer.set_closed_promise(WebIDL::create_resolved_promise(realm, JS::js_undefined()));
    }
    // 8. Otherwise,
    else {
        // 1. Assert: state is "errored".
        VERIFY(state == WritableStream::State::Errored);

        // 2. Let storedError be stream.[[storedError]].
        auto stored_error = stream.stored_error();

        // 3. Set writer.[[readyPromise]] to a promise rejected with storedError.
        writer.set_ready_promise(WebIDL::create_rejected_promise(realm, stored_error));

        // 4. Set writer.[[readyPromise]].[[PromiseIsHandled]] to true.
        WebIDL::mark_promise_as_handled(*writer.ready_promise());

        // 5. Set writer.[[closedPromise]] to a promise rejected with storedError.
        writer.set_closed_promise(WebIDL::create_rejected_promise(realm, stored_error));

        // 6. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
        WebIDL::mark_promise_as_handled(*writer.closed_promise());
    }

    return {};
}

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller(ReadableStream& stream, ReadableByteStreamController& controller, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm, double high_water_mark, JS::Value auto_allocate_chunk_size)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[controller]] is undefined.
    VERIFY(!stream.controller().has_value());

    // 2. If autoAllocateChunkSize is not undefined,
    if (!auto_allocate_chunk_size.is_undefined()) {
        // 1. Assert: ! IsInteger(autoAllocateChunkSize) is true.
        VERIFY(auto_allocate_chunk_size.is_integral_number());

        // 2. Assert: autoAllocateChunkSize is positive.
        VERIFY(auto_allocate_chunk_size.as_double() > 0);
    }

    // 3. Set controller.[[stream]] to stream.
    controller.set_stream(stream);

    // 4. Set controller.[[pullAgain]] and controller.[[pulling]] to false.
    controller.set_pull_again(false);
    controller.set_pulling(false);

    // 5. Set controller.[[byobRequest]] to null.
    controller.set_byob_request({});

    // 6. Perform ! ResetQueue(controller).
    reset_queue(controller);

    // 7. Set controller.[[closeRequested]] and controller.[[started]] to false.
    controller.set_close_requested(false);
    controller.set_started(false);

    // 8. Set controller.[[strategyHWM]] to highWaterMark.
    controller.set_strategy_hwm(high_water_mark);

    // 9. Set controller.[[pullAlgorithm]] to pullAlgorithm.
    controller.set_pull_algorithm(pull_algorithm);

    // 10. Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
    controller.set_cancel_algorithm(cancel_algorithm);

    // 11. Set controller.[[autoAllocateChunkSize]] to autoAllocateChunkSize.
    if (auto_allocate_chunk_size.is_integral_number())
        controller.set_auto_allocate_chunk_size(auto_allocate_chunk_size.as_double());

    // 12. Set controller.[[pendingPullIntos]] to a new empty list.
    controller.pending_pull_intos().clear();

    // 13. Set stream.[[controller]] to controller.
    stream.set_controller(ReadableStreamController { controller });

    // 14. Let startResult be the result of performing startAlgorithm.
    auto start_result = TRY(start_algorithm->function()());

    // 15. Let startPromise be a promise resolved with startResult.
    auto start_promise = WebIDL::create_resolved_promise(realm, start_result);

    // 16. Upon fulfillment of startPromise,
    WebIDL::upon_fulfillment(start_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Set controller.[[started]] to true.
        controller.set_started(true);

        // 2. Assert: controller.[[pulling]] is false.
        VERIFY(!controller.pulling());

        // 3. Assert: controller.[[pullAgain]] is false.
        VERIFY(!controller.pull_again());

        // 4. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
        readable_byte_stream_controller_call_pull_if_needed(controller);

        return JS::js_undefined();
    }));

    // 17. Upon rejection of startPromise with reason r,
    WebIDL::upon_rejection(start_promise, JS::create_heap_function(controller.heap(), [&controller](JS::Value r) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform ! ReadableByteStreamControllerError(controller, r).
        readable_byte_stream_controller_error(controller, r);

        return JS::js_undefined();
    }));

    return {};
}

// https://streams.spec.whatwg.org/#readablestream-enqueue
WebIDL::ExceptionOr<void> readable_stream_enqueue(ReadableStreamController& controller, JS::Value chunk)
{
    // 1. If stream.[[controller]] implements ReadableStreamDefaultController,
    if (controller.has<JS::NonnullGCPtr<ReadableStreamDefaultController>>()) {
        // 1. Perform ! ReadableStreamDefaultControllerEnqueue(stream.[[controller]], chunk).
        return readable_stream_default_controller_enqueue(controller.get<JS::NonnullGCPtr<ReadableStreamDefaultController>>(), chunk);
    }
    // 2. Otherwise,
    else {
        // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
        VERIFY(controller.has<JS::NonnullGCPtr<ReadableByteStreamController>>());
        auto readable_byte_controller = controller.get<JS::NonnullGCPtr<ReadableByteStreamController>>();

        // FIXME: 2. Assert: chunk is an ArrayBufferView.

        // 3. Let byobView be the current BYOB request view for stream.
        // FIXME: This is not what the spec means by 'current BYOB request view'
        auto byob_view = readable_byte_controller->raw_byob_request();

        // 4. If byobView is non-null, and chunk.[[ViewedArrayBuffer]] is byobView.[[ViewedArrayBuffer]], then:
        if (byob_view) {
            // FIXME: 1. Assert: chunk.[[ByteOffset]] is byobView.[[ByteOffset]].
            // FIXME: 2. Assert: chunk.[[ByteLength]] ≤ byobView.[[ByteLength]].
            // FIXME: 3. Perform ? ReadableByteStreamControllerRespond(stream.[[controller]], chunk.[[ByteLength]]).
            TODO();
        }

        // 5. Otherwise, perform ? ReadableByteStreamControllerEnqueue(stream.[[controller]], chunk).
        return readable_byte_stream_controller_enqueue(readable_byte_controller, chunk);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue(ReadableByteStreamController& controller, JS::Value chunk)
{
    auto& vm = controller.vm();
    auto& realm = controller.realm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If controller.[[closeRequested]] is true or stream.[[state]] is not "readable", return.
    if (controller.close_requested() || stream->state() != ReadableStream ::State::Readable)
        return {};

    // 3. Let buffer be chunk.[[ViewedArrayBuffer]].
    auto* typed_array = TRY(JS::typed_array_from(vm, chunk));
    auto* buffer = typed_array->viewed_array_buffer();

    // 4. Let byteOffset be chunk.[[ByteOffset]].
    auto byte_offset = typed_array->byte_offset();

    // 6. If ! IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    // FIXME: The streams spec has not been updated for resizable ArrayBuffer objects. We must perform step 6 before
    //        invoking TypedArrayByteLength in step 5. We also must check if the array is out-of-bounds, rather than
    //        just detached.
    auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(*typed_array, JS::ArrayBuffer::Order::SeqCst);

    if (JS::is_typed_array_out_of_bounds(typed_array_record))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 5. Let byteLength be chunk.[[ByteLength]].
    auto byte_length = JS::typed_array_byte_length(typed_array_record);

    // 7. Let transferredBuffer be ? TransferArrayBuffer(buffer).
    auto transferred_buffer = TRY(transfer_array_buffer(realm, *buffer));

    // 8. If controller.[[pendingPullIntos]] is not empty,
    if (!controller.pending_pull_intos().is_empty()) {
        // 1. Let firstPendingPullInto be controller.[[pendingPullIntos]][0].
        auto& first_pending_pull_into = controller.pending_pull_intos().first();

        // 2. If ! IsDetachedBuffer(firstPendingPullInto’s buffer) is true, throw a TypeError exception.
        if (first_pending_pull_into.buffer->is_detached()) {
            auto error = JS::TypeError::create(realm, "Buffer is detached"sv);
            return JS::throw_completion(error);
        }

        // 3. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
        readable_byte_stream_controller_invalidate_byob_request(controller);

        // 4. Set firstPendingPullInto’s buffer to ! TransferArrayBuffer(firstPendingPullInto’s buffer).
        first_pending_pull_into.buffer = MUST(transfer_array_buffer(realm, first_pending_pull_into.buffer));

        // 5. If firstPendingPullInto’s reader type is "none", perform ? ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, firstPendingPullInto).
        if (first_pending_pull_into.reader_type == ReaderType::None)
            TRY(readable_byte_stream_controller_enqueue_detached_pull_into_queue(controller, first_pending_pull_into));
    }

    // 9. If ! ReadableStreamHasDefaultReader(stream) is true,
    if (readable_stream_has_default_reader(*stream)) {
        // 1. Perform ! ReadableByteStreamControllerProcessReadRequestsUsingQueue(controller).
        readable_byte_stream_controller_process_read_requests_using_queue(controller);

        // 2. If ! ReadableStreamGetNumReadRequests(stream) is 0,
        if (readable_stream_get_num_read_requests(*stream) == 0) {
            // 1. Assert: controller.[[pendingPullIntos]] is empty.
            VERIFY(controller.pending_pull_intos().is_empty());

            // 2. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset, byteLength).
            readable_byte_stream_controller_enqueue_chunk_to_queue(controller, transferred_buffer, byte_offset, byte_length);
        }
        // 3. Otherwise.
        else {
            // 1. Assert: controller.[[queue]] is empty.
            VERIFY(controller.queue().is_empty());

            // 2. If controller.[[pendingPullIntos]] is not empty,
            if (!controller.pending_pull_intos().is_empty()) {
                // 1. Assert: controller.[[pendingPullIntos]][0]'s reader type is "default".
                VERIFY(controller.pending_pull_intos().first().reader_type == ReaderType::Default);

                // 2. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
                readable_byte_stream_controller_shift_pending_pull_into(controller);
            }

            // 3. Let transferredView be ! Construct(%Uint8Array%, « transferredBuffer, byteOffset, byteLength »).
            auto transferred_view = MUST(JS::construct(vm, *realm.intrinsics().uint8_array_constructor(), transferred_buffer, JS::Value(byte_offset), JS::Value(byte_length)));

            // 4. Perform ! ReadableStreamFulfillReadRequest(stream, transferredView, false).
            readable_stream_fulfill_read_request(*stream, transferred_view, false);
        }
    }
    // 10. Otherwise, if ! ReadableStreamHasBYOBReader(stream) is true,
    else if (readable_stream_has_byob_reader(*stream)) {
        // 1. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset, byteLength).
        readable_byte_stream_controller_enqueue_chunk_to_queue(controller, transferred_buffer, byte_offset, byte_length);

        // 2. Perform ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
        readable_byte_stream_controller_process_pull_into_descriptors_using_queue(controller);
    }
    // 11. Otherwise,
    else {
        // 1. Assert: ! IsReadableStreamLocked(stream) is false.
        VERIFY(!is_readable_stream_locked(*stream));

        // 2. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset, byteLength).
        readable_byte_stream_controller_enqueue_chunk_to_queue(controller, transferred_buffer, byte_offset, byte_length);
    }

    // 12. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
    readable_byte_stream_controller_call_pull_if_needed(controller);

    return {};
}

// https://streams.spec.whatwg.org/#readablestream-pull-from-bytes
WebIDL::ExceptionOr<void> readable_stream_pull_from_bytes(ReadableStream& stream, ByteBuffer bytes)
{
    // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
    auto controller = stream.controller()->get<JS::NonnullGCPtr<ReadableByteStreamController>>();

    // 2. Let available be bytes’s length.
    auto available = bytes.size();

    // 3. Let desiredSize be available.
    auto desired_size = available;

    // FIXME: 4. If stream’s current BYOB request view is non-null, then set desiredSize to stream’s current BYOB request
    //           view's byte length.

    // 5. Let pullSize be the smaller value of available and desiredSize.
    auto pull_size = min(available, desired_size);

    // 6. Let pulled be the first pullSize bytes of bytes.
    auto pulled = pull_size == available ? move(bytes) : MUST(bytes.slice(0, pull_size));

    // 7. Remove the first pullSize bytes from bytes.
    if (pull_size != available)
        bytes = MUST(bytes.slice(pull_size, available - pull_size));

    // FIXME: 8. If stream’s current BYOB request view is non-null, then:
    //           1. Write pulled into stream’s current BYOB request view.
    //           2. Perform ? ReadableByteStreamControllerRespond(stream.[[controller]], pullSize).
    // 9. Otherwise,
    {
        auto& realm = HTML::relevant_realm(stream);

        // 1. Set view to the result of creating a Uint8Array from pulled in stream’s relevant Realm.
        auto array_buffer = JS::ArrayBuffer::create(realm, move(pulled));
        auto view = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

        // 2. Perform ? ReadableByteStreamControllerEnqueue(stream.[[controller]], view).
        TRY(readable_byte_stream_controller_enqueue(controller, view));
    }

    return {};
}

// https://streams.spec.whatwg.org/#transfer-array-buffer
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> transfer_array_buffer(JS::Realm& realm, JS::ArrayBuffer& buffer)
{
    auto& vm = realm.vm();

    // 1. Assert: ! IsDetachedBuffer(O) is false.
    VERIFY(!buffer.is_detached());

    // 2. Let arrayBufferData be O.[[ArrayBufferData]].
    // 3. Let arrayBufferByteLength be O.[[ArrayBufferByteLength]].
    auto array_buffer = buffer.buffer();

    // 4. Perform ? DetachArrayBuffer(O).
    TRY(JS::detach_array_buffer(vm, buffer));

    // 5. Return a new ArrayBuffer object, created in the current Realm, whose [[ArrayBufferData]] internal slot value is arrayBufferData and whose [[ArrayBufferByteLength]] internal slot value is arrayBufferByteLength.
    return JS::ArrayBuffer::create(realm, move(array_buffer));
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueuedetachedpullintotoqueue
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue_detached_pull_into_queue(ReadableByteStreamController& controller, PullIntoDescriptor& pull_into_descriptor)
{
    // 1. Assert: pullIntoDescriptor’s reader type is "none".
    VERIFY(pull_into_descriptor.reader_type == ReaderType::None);

    // 2. If pullIntoDescriptor’s bytes filled > 0, perform ? ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller, pullIntoDescriptor’s buffer, pullIntoDescriptor’s byte offset, pullIntoDescriptor’s bytes filled).
    if (pull_into_descriptor.bytes_filled > 0)
        TRY(readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(controller, pull_into_descriptor.buffer, pull_into_descriptor.byte_offset, pull_into_descriptor.bytes_filled));

    // 3. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
    readable_byte_stream_controller_shift_pending_pull_into(controller);
    return {};
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-commit-pull-into-descriptor
void readable_byte_stream_controller_commit_pull_into_descriptor(ReadableStream& stream, PullIntoDescriptor const& pull_into_descriptor)
{
    // 1. Assert: stream.[[state]] is not "errored".
    VERIFY(!stream.is_errored());

    // 2. Assert: pullIntoDescriptor.reader type is not "none".
    VERIFY(pull_into_descriptor.reader_type != ReaderType::None);

    // 3. Let done be false.
    bool done = false;

    // 4. If stream.[[state]] is "closed",
    if (stream.is_closed()) {
        // 1. Assert: pullIntoDescriptor’s bytes filled is 0.
        VERIFY(pull_into_descriptor.bytes_filled == 0);

        // 2. Set done to true.
        done = true;
    }

    // 5. Let filledView be ! ReadableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor).
    auto filled_view = readable_byte_stream_controller_convert_pull_into_descriptor(stream.realm(), pull_into_descriptor);

    // 6. If pullIntoDescriptor’s reader type is "default",
    if (pull_into_descriptor.reader_type == ReaderType::Default) {
        // 1. Perform ! ReadableStreamFulfillReadRequest(stream, filledView, done).
        readable_stream_fulfill_read_request(stream, filled_view, done);
    }
    // 7. Otherwise,
    else {
        // 1. Assert: pullIntoDescriptor’s reader type is "byob".
        VERIFY(pull_into_descriptor.reader_type == ReaderType::Byob);

        // 2. Perform ! ReadableStreamFulfillReadIntoRequest(stream, filledView, done).
        readable_stream_fulfill_read_into_request(stream, filled_view, done);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-process-pull-into-descriptors-using-queue
void readable_byte_stream_controller_process_pull_into_descriptors_using_queue(ReadableByteStreamController& controller)
{
    // 1. Assert: controller.[[closeRequested]] is false.
    VERIFY(!controller.close_requested());

    // 2. While controller.[[pendingPullIntos]] is not empty,
    while (!controller.pending_pull_intos().is_empty()) {
        // 1. If controller.[[queueTotalSize]] is 0, return.
        if (controller.queue_total_size() == 0)
            return;

        // 2. Let pullIntoDescriptor be controller.[[pendingPullIntos]][0].
        auto& pull_into_descriptor = controller.pending_pull_intos().first();

        // 3. If ! ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, pullIntoDescriptor) is true,
        if (readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(controller, pull_into_descriptor)) {
            // NOTE: We store the returned pull into descriptor here as the 'shift pending pull into' will remove
            //       the first entry into the list which we have a reference to above.

            // 1. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
            auto descriptor = readable_byte_stream_controller_shift_pending_pull_into(controller);

            // 2. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], pullIntoDescriptor).
            readable_byte_stream_controller_commit_pull_into_descriptor(*controller.stream(), descriptor);
        }
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerprocessreadrequestsusingqueue
void readable_byte_stream_controller_process_read_requests_using_queue(ReadableByteStreamController& controller)
{
    // 1. Let reader be controller.[[stream]].[[reader]].
    auto reader = controller.stream()->reader();

    // 2. Assert: reader implements ReadableStreamDefaultReader.
    VERIFY(reader->has<JS::NonnullGCPtr<ReadableStreamDefaultReader>>());

    // 3. While reader.[[readRequests]] is not empty,
    auto readable_stream_default_reader = reader->get<JS::NonnullGCPtr<ReadableStreamDefaultReader>>();
    while (!readable_stream_default_reader->read_requests().is_empty()) {
        // 1. If controller.[[queueTotalSize]] is 0, return.
        if (controller.queue_total_size() == 0.0)
            return;

        // 2. Let readRequest be reader.[[readRequests]][0].
        // 3. Remove readRequest from reader.[[readRequests]].
        auto read_request = readable_stream_default_reader->read_requests().take_first();

        // 4. Perform ! ReadableByteStreamControllerFillReadRequestFromQueue(controller, readRequest).
        readable_byte_stream_controller_fill_read_request_from_queue(controller, read_request);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue-chunk-to-queue
void readable_byte_stream_controller_enqueue_chunk_to_queue(ReadableByteStreamController& controller, JS::NonnullGCPtr<JS::ArrayBuffer> buffer, u32 byte_offset, u32 byte_length)
{
    // 1. Append a new readable byte stream queue entry with buffer buffer, byte offset byteOffset, and byte length byteLength to controller.[[queue]].
    controller.queue().append(ReadableByteStreamQueueEntry {
        .buffer = buffer,
        .byte_offset = byte_offset,
        .byte_length = byte_length,
    });

    // 2. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] + byteLength.
    controller.set_queue_total_size(controller.queue_total_size() + byte_length);
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueueclonedchunktoqueue
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(ReadableByteStreamController& controller, JS::ArrayBuffer& buffer, u64 byte_offset, u64 byte_length)
{
    auto& vm = controller.vm();

    // 1. Let cloneResult be CloneArrayBuffer(buffer, byteOffset, byteLength, %ArrayBuffer%).
    auto clone_result = JS::clone_array_buffer(vm, buffer, byte_offset, byte_length);

    // 2. If cloneResult is an abrupt completion,
    if (clone_result.is_throw_completion()) {
        auto throw_completion = Bindings::throw_dom_exception_if_needed(vm, [&] { return clone_result; }).throw_completion();

        // 1. Perform ! ReadableByteStreamControllerError(controller, cloneResult.[[Value]]).
        readable_byte_stream_controller_error(controller, throw_completion.value().value());

        // 2. Return cloneResult.
        // Note: We need to return the throw_completion object here, as enqueue needs to throw the same object that the controller is errored with
        return throw_completion;
    }

    // 3. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, cloneResult.[[Value]], 0, byteLength).
    readable_byte_stream_controller_enqueue_chunk_to_queue(controller, *clone_result.release_value(), 0, byte_length);

    return {};
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-shift-pending-pull-into
PullIntoDescriptor readable_byte_stream_controller_shift_pending_pull_into(ReadableByteStreamController& controller)
{
    // 1. Assert: controller.[[byobRequest]] is null.
    VERIFY(!controller.raw_byob_request());

    // 2. Let descriptor be controller.[[pendingPullIntos]][0].
    // 3. Remove descriptor from controller.[[pendingPullIntos]].
    auto descriptor = controller.pending_pull_intos().take_first();

    // 4. Return descriptor.
    return descriptor;
}

// https://streams.spec.whatwg.org/#readablestream-set-up-with-byte-reading-support
void set_up_readable_stream_controller_with_byte_reading_support(ReadableStream& stream, JS::GCPtr<PullAlgorithm> pull_algorithm, JS::GCPtr<CancelAlgorithm> cancel_algorithm, double high_water_mark)
{
    auto& realm = stream.realm();

    // 1. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = JS::create_heap_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); });

    // 2. Let pullAlgorithmWrapper be an algorithm that runs these steps:
    auto pull_algorithm_wrapper = JS::create_heap_function(realm.heap(), [&realm, pull_algorithm]() {
        // 1. Let result be the result of running pullAlgorithm, if pullAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        JS::GCPtr<JS::PromiseCapability> result = nullptr;
        if (pull_algorithm)
            result = pull_algorithm->function()();

        // 2. If result is a Promise, then return result.
        if (result != nullptr)
            return JS::NonnullGCPtr(*result);

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 3. Let cancelAlgorithmWrapper be an algorithm that runs these steps:
    auto cancel_algorithm_wrapper = JS::create_heap_function(realm.heap(), [&realm, cancel_algorithm](JS::Value c) {
        // 1. Let result be the result of running cancelAlgorithm, if cancelAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        JS::GCPtr<JS::PromiseCapability> result = nullptr;
        if (cancel_algorithm)
            result = cancel_algorithm->function()(c);

        // 2. If result is a Promise, then return result.
        if (result != nullptr)
            return JS::NonnullGCPtr(*result);

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Perform ! InitializeReadableStream(stream).
    // 5. Let controller be a new ReadableByteStreamController.
    auto controller = stream.heap().allocate<ReadableByteStreamController>(realm, realm);

    // 6. Perform ! SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithmWrapper, cancelAlgorithmWrapper, highWaterMark, undefined).
    MUST(set_up_readable_byte_stream_controller(stream, controller, start_algorithm, pull_algorithm_wrapper, cancel_algorithm_wrapper, high_water_mark, JS::js_undefined()));
}

// https://streams.spec.whatwg.org/#writable-stream-abort
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_abort(WritableStream& stream, JS::Value reason)
{
    auto& realm = stream.realm();

    // 1. If stream.[[state]] is "closed" or "errored", return a promise resolved with undefined.
    auto state = stream.state();
    if (state == WritableStream::State::Closed || state == WritableStream::State::Errored)
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 2. Signal abort on stream.[[controller]].[[signal]] with reason.
    stream.controller()->signal()->signal_abort(reason);

    // 3. Let state be stream.[[state]].
    state = stream.state();

    // 4. If state is "closed" or "errored", return a promise resolved with undefined.
    if (state == WritableStream::State::Closed || state == WritableStream::State::Errored)
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 5. If stream.[[pendingAbortRequest]] is not undefined, return stream.[[pendingAbortRequest]]'s promise.
    if (stream.pending_abort_request().has_value())
        return stream.pending_abort_request()->promise;

    // 6. Assert: state is "writable" or "erroring".
    VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

    // 7. Let wasAlreadyErroring be false.
    auto was_already_erroring = false;

    // 8. If state is "erroring",
    if (state == WritableStream::State::Erroring) {
        // 1. Set wasAlreadyErroring to true.
        was_already_erroring = true;

        // 2. Set reason to undefined.
        reason = JS::js_undefined();
    }

    // 9. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 10. Set stream.[[pendingAbortRequest]] to a new pending abort request whose promise is promise, reason is reason, and was already erroring is wasAlreadyErroring.
    stream.set_pending_abort_request(PendingAbortRequest { promise, reason, was_already_erroring });

    // 11. If wasAlreadyErroring is false, perform ! WritableStreamStartErroring(stream, reason).
    if (!was_already_erroring)
        writable_stream_start_erroring(stream, reason);

    // 12. Return promise.
    return promise;
}

// https://streams.spec.whatwg.org/#writable-stream-close
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_close(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let state be stream.[[state]].
    auto state = stream.state();

    // 2. If state is "closed" or "errored", return a promise rejected with a TypeError exception.
    if (state == WritableStream::State::Closed || state == WritableStream::State::Errored) {
        auto message = state == WritableStream::State::Closed ? "Cannot close a closed stream"sv : "Cannot close an errored stream"sv;
        auto exception = JS::TypeError::create(realm, message);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 3. Assert: state is "writable" or "erroring".
    VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

    // 4. Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
    VERIFY(!writable_stream_close_queued_or_in_flight(stream));

    // 5. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Set stream.[[closeRequest]] to promise.
    stream.set_close_request(promise);

    // 7. Let writer be stream.[[writer]].
    auto writer = stream.writer();

    // 8. If writer is not undefined, and stream.[[backpressure]] is true, and state is "writable", resolve writer.[[readyPromise]] with undefined.
    if (writer && stream.backpressure() && state == WritableStream::State::Writable)
        WebIDL::resolve_promise(realm, *writer->ready_promise(), JS::js_undefined());

    // 9. Perform ! WritableStreamDefaultControllerClose(stream.[[controller]]).
    writable_stream_default_controller_close(*stream.controller());

    // 10. Return promise.
    return promise;
}

// https://streams.spec.whatwg.org/#writable-stream-add-write-request
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_add_write_request(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Assert: ! IsWritableStreamLocked(stream) is true.
    VERIFY(is_writable_stream_locked(stream));

    // 2. Assert: stream.[[state]] is "writable".
    VERIFY(stream.state() == WritableStream::State::Writable);

    // 3. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 4. Append promise to stream.[[writeRequests]].
    stream.write_requests().append(promise);

    // 5. Return promise.
    return promise;
}

// https://streams.spec.whatwg.org/#writable-stream-close-queued-or-in-flight
bool writable_stream_close_queued_or_in_flight(WritableStream const& stream)
{
    // 1. If stream.[[closeRequest]] is undefined and stream.[[inFlightCloseRequest]] is undefined, return false.
    if (!stream.close_request() && !stream.in_flight_close_request())
        return false;

    // 2. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#writable-stream-deal-with-rejection
void writable_stream_deal_with_rejection(WritableStream& stream, JS::Value error)
{
    // 1. Let state be stream.[[state]].
    auto state = stream.state();

    // 2. If state is "writable",
    if (state == WritableStream::State::Writable) {
        // 1. Perform ! WritableStreamStartErroring(stream, error).
        writable_stream_start_erroring(stream, error);

        // 2. Return.
        return;
    }

    // 3. Assert: state is "erroring".
    VERIFY(state == WritableStream::State::Erroring);

    // 4. Perform ! WritableStreamFinishErroring(stream).
    writable_stream_finish_erroring(stream);
}

// https://streams.spec.whatwg.org/#writable-stream-finish-erroring
void writable_stream_finish_erroring(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[state]] is "erroring".
    VERIFY(stream.state() == WritableStream::State::Erroring);

    // 2. Assert: ! WritableStreamHasOperationMarkedInFlight(stream) is false.
    VERIFY(!writable_stream_has_operation_marked_in_flight(stream));

    // 3. Set stream.[[state]] to "errored".
    stream.set_state(WritableStream::State::Errored);

    // 4. Perform ! stream.[[controller]].[[ErrorSteps]]().
    stream.controller()->error_steps();

    // 5. Let storedError be stream.[[storedError]].
    auto stored_error = stream.stored_error();

    // 6. For each writeRequest of stream.[[writeRequests]]:
    for (auto& write_request : stream.write_requests()) {
        // 1. Reject writeRequest with storedError.
        WebIDL::reject_promise(realm, *write_request, stored_error);
    }

    // 7. Set stream.[[writeRequests]] to an empty list.
    stream.write_requests().clear();

    // 8. If stream.[[pendingAbortRequest]] is undefined,
    if (!stream.pending_abort_request().has_value()) {
        // 1. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
        writable_stream_reject_close_and_closed_promise_if_needed(stream);

        // 2. Return.
        return;
    }

    // 9. Let abortRequest be stream.[[pendingAbortRequest]].
    // 10. Set stream.[[pendingAbortRequest]] to undefined.
    auto abort_request = stream.pending_abort_request().release_value();

    // 11. If abortRequest’s was already erroring is true,
    if (abort_request.was_already_erroring) {
        // 1. Reject abortRequest’s promise with storedError.
        WebIDL::reject_promise(realm, abort_request.promise, stored_error);

        // 2. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
        writable_stream_reject_close_and_closed_promise_if_needed(stream);

        // 3. Return.
        return;
    }

    // 12. Let promise be ! stream.[[controller]].[[AbortSteps]](abortRequest’s reason).
    auto promise = stream.controller()->abort_steps(abort_request.reason);

    // 13. Upon fulfillment of promise,
    WebIDL::upon_fulfillment(*promise, JS::create_heap_function(realm.heap(), [&realm, &stream, abort_promise = abort_request.promise](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Resolve abortRequest’s promise with undefined.
        WebIDL::resolve_promise(realm, abort_promise, JS::js_undefined());

        // 2. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
        writable_stream_reject_close_and_closed_promise_if_needed(stream);

        return JS::js_undefined();
    }));

    // 14. Upon rejection of promise with reason reason,
    WebIDL::upon_rejection(*promise, JS::create_heap_function(realm.heap(), [&realm, &stream, abort_promise = abort_request.promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Reject abortRequest’s promise with reason.
        WebIDL::reject_promise(realm, abort_promise, reason);

        // 2. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
        writable_stream_reject_close_and_closed_promise_if_needed(stream);

        return JS::js_undefined();
    }));
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close
void writable_stream_finish_in_flight_close(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
    VERIFY(stream.in_flight_close_request());

    // 2. Resolve stream.[[inFlightCloseRequest]] with undefined.
    WebIDL::resolve_promise(realm, *stream.in_flight_close_request(), JS::js_undefined());

    // 3. Set stream.[[inFlightCloseRequest]] to undefined.
    stream.set_in_flight_close_request({});

    // 4. Let state be stream.[[state]].
    auto state = stream.state();

    // 5. Assert: stream.[[state]] is "writable" or "erroring".
    VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

    // 6. If state is "erroring",
    if (state == WritableStream::State::Erroring) {
        // 1. Set stream.[[storedError]] to undefined.
        stream.set_stored_error(JS::js_undefined());

        // 2. If stream.[[pendingAbortRequest]] is not undefined,
        if (stream.pending_abort_request().has_value()) {
            // 1. Resolve stream.[[pendingAbortRequest]]'s promise with undefined.
            // 2. Set stream.[[pendingAbortRequest]] to undefined.
            WebIDL::resolve_promise(realm, stream.pending_abort_request().release_value().promise, JS::js_undefined());
        }
    }

    // 7. Set stream.[[state]] to "closed".
    stream.set_state(WritableStream::State::Closed);

    // 8. Let writer be stream.[[writer]].
    auto writer = stream.writer();

    // 9. If writer is not undefined, resolve writer.[[closedPromise]] with undefined.
    if (writer)
        WebIDL::resolve_promise(realm, *writer->closed_promise(), JS::js_undefined());

    // 10. Assert: stream.[[pendingAbortRequest]] is undefined.
    VERIFY(!stream.pending_abort_request().has_value());

    // 11. Assert: stream.[[storedError]] is undefined.
    VERIFY(stream.stored_error().is_undefined());
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close-with-error
void writable_stream_finish_in_flight_close_with_error(WritableStream& stream, JS::Value error)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
    VERIFY(stream.in_flight_close_request());

    // 2. Reject stream.[[inFlightCloseRequest]] with error.
    WebIDL::reject_promise(realm, *stream.in_flight_close_request(), error);

    // 3. Set stream.[[inFlightCloseRequest]] to undefined.
    stream.set_in_flight_close_request({});

    // 4. Assert: stream.[[state]] is "writable" or "erroring".
    auto state = stream.state();
    VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

    // 5. If stream.[[pendingAbortRequest]] is not undefined,
    if (stream.pending_abort_request().has_value()) {
        // 1. Reject stream.[[pendingAbortRequest]]'s promise with error.
        // 2. Set stream.[[pendingAbortRequest]] to undefined.
        WebIDL::reject_promise(realm, stream.pending_abort_request().release_value().promise, error);
    }

    // 6. Perform ! WritableStreamDealWithRejection(stream, error).
    writable_stream_deal_with_rejection(stream, error);
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write
void writable_stream_finish_in_flight_write(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
    VERIFY(stream.in_flight_write_request());

    // 2. Resolve stream.[[inFlightWriteRequest]] with undefined.
    WebIDL::resolve_promise(realm, *stream.in_flight_write_request(), JS::js_undefined());

    // 3. Set stream.[[inFlightWriteRequest]] to undefined.
    stream.set_in_flight_write_request({});
}

// https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write-with-error
void writable_stream_finish_in_flight_write_with_error(WritableStream& stream, JS::Value error)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
    VERIFY(stream.in_flight_write_request());

    // 2. Reject stream.[[inFlightWriteRequest]] with error.
    WebIDL::reject_promise(realm, *stream.in_flight_write_request(), error);

    // 3. Set stream.[[inFlightWriteRequest]] to undefined.
    stream.set_in_flight_write_request({});

    // 4. Assert: stream.[[state]] is "writable" or "erroring".
    auto state = stream.state();
    VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

    // 5. Perform ! WritableStreamDealWithRejection(stream, error).
    writable_stream_deal_with_rejection(stream, error);
}

// https://streams.spec.whatwg.org/#writable-stream-has-operation-marked-in-flight
bool writable_stream_has_operation_marked_in_flight(WritableStream const& stream)
{
    // 1. If stream.[[inFlightWriteRequest]] is undefined and stream.[[inFlightCloseRequest]] is undefined, return false.
    if (!stream.in_flight_write_request() && !stream.in_flight_close_request())
        return false;

    // 2. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#writable-stream-mark-close-request-in-flight
void writable_stream_mark_close_request_in_flight(WritableStream& stream)
{
    // 1. Assert: stream.[[inFlightCloseRequest]] is undefined.
    VERIFY(!stream.in_flight_close_request());

    // 2. Assert: stream.[[closeRequest]] is not undefined.
    VERIFY(stream.close_request());

    // 3. Set stream.[[inFlightCloseRequest]] to stream.[[closeRequest]].
    stream.set_in_flight_close_request(stream.close_request());

    // 4. Set stream.[[closeRequest]] to undefined.
    stream.set_close_request({});
}

// https://streams.spec.whatwg.org/#writable-stream-mark-first-write-request-in-flight
void writable_stream_mark_first_write_request_in_flight(WritableStream& stream)
{
    // 1. Assert: stream.[[inFlightWriteRequest]] is undefined.
    VERIFY(!stream.in_flight_write_request());

    // 2. Assert: stream.[[writeRequests]] is not empty.
    VERIFY(!stream.write_requests().is_empty());

    // 3. Let writeRequest be stream.[[writeRequests]][0].
    // 4. Remove writeRequest from stream.[[writeRequests]].
    auto write_request = stream.write_requests().take_first();

    // 5. Set stream.[[inFlightWriteRequest]] to writeRequest.
    stream.set_in_flight_write_request(write_request);
}

// https://streams.spec.whatwg.org/#writable-stream-reject-close-and-closed-promise-if-needed
void writable_stream_reject_close_and_closed_promise_if_needed(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[state]] is "errored".
    VERIFY(stream.state() == WritableStream::State::Errored);

    // 2. If stream.[[closeRequest]] is not undefined,
    if (stream.close_request()) {
        // 1. Assert: stream.[[inFlightCloseRequest]] is undefined.
        VERIFY(!stream.in_flight_close_request());

        // 2. Reject stream.[[closeRequest]] with stream.[[storedError]].
        WebIDL::reject_promise(realm, *stream.close_request(), stream.stored_error());

        // 3. Set stream.[[closeRequest]] to undefined.
        stream.set_close_request({});
    }

    // 3. Let writer be stream.[[writer]].
    auto writer = stream.writer();

    // 4. If writer is not undefined,
    if (writer) {
        // 1. Reject writer.[[closedPromise]] with stream.[[storedError]].
        WebIDL::reject_promise(realm, *writer->closed_promise(), stream.stored_error());

        // 2. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
        WebIDL::mark_promise_as_handled(*writer->closed_promise());
    }
}

// https://streams.spec.whatwg.org/#writable-stream-start-erroring
void writable_stream_start_erroring(WritableStream& stream, JS::Value reason)
{
    // 1. Assert: stream.[[storedError]] is undefined.
    VERIFY(stream.stored_error().is_undefined());

    // 2. Assert: stream.[[state]] is "writable".
    VERIFY(stream.state() == WritableStream::State::Writable);

    // 3. Let controller be stream.[[controller]].
    auto controller = stream.controller();

    // 4. Assert: controller is not undefined.
    VERIFY(controller);

    // 5. Set stream.[[state]] to "erroring".
    stream.set_state(WritableStream::State::Erroring);

    // 6. Set stream.[[storedError]] to reason.
    stream.set_stored_error(reason);

    // 7. Let writer be stream.[[writer]].
    auto writer = stream.writer();

    // 8. If writer is not undefined, perform ! WritableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason).
    if (writer)
        writable_stream_default_writer_ensure_ready_promise_rejected(*writer, reason);

    // 9. If ! WritableStreamHasOperationMarkedInFlight(stream) is false and controller.[[started]] is true, perform ! WritableStreamFinishErroring(stream).
    if (!writable_stream_has_operation_marked_in_flight(stream) && controller->started())
        writable_stream_finish_erroring(stream);
}

// https://streams.spec.whatwg.org/#writable-stream-update-backpressure
void writable_stream_update_backpressure(WritableStream& stream, bool backpressure)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[state]] is "writable".
    VERIFY(stream.state() == WritableStream::State::Writable);

    // 2. Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
    VERIFY(!writable_stream_close_queued_or_in_flight(stream));

    // 3. Let writer be stream.[[writer]].
    auto writer = stream.writer();

    // 4. If writer is not undefined and backpressure is not stream.[[backpressure]],
    if (writer && backpressure != stream.backpressure()) {
        // 1. If backpressure is true, set writer.[[readyPromise]] to a new promise.
        if (backpressure) {
            writer->set_ready_promise(WebIDL::create_promise(realm));
        }
        // 2. Otherwise,
        else {
            // 1. Assert: backpressure is false.

            // 2. Resolve writer.[[readyPromise]] with undefined.
            WebIDL::resolve_promise(realm, *writer->ready_promise(), JS::js_undefined());
        }
    }

    // 5. Set stream.[[backpressure]] to backpressure.
    stream.set_backpressure(backpressure);
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-abort
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_default_writer_abort(WritableStreamDefaultWriter& writer, JS::Value reason)
{
    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Return ! WritableStreamAbort(stream, reason).
    return writable_stream_abort(*stream, reason);
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-close
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_default_writer_close(WritableStreamDefaultWriter& writer)
{
    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Return ! WritableStreamClose(stream).
    return writable_stream_close(*stream);
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-ensure-closed-promise-rejected
void writable_stream_default_writer_ensure_closed_promise_rejected(WritableStreamDefaultWriter& writer, JS::Value error)
{
    auto& realm = writer.realm();

    // 1. If writer.[[closedPromise]].[[PromiseState]] is "pending", reject writer.[[closedPromise]] with error.
    auto& closed_promise = verify_cast<JS::Promise>(*writer.closed_promise()->promise());
    if (closed_promise.state() == JS::Promise::State::Pending) {
        WebIDL::reject_promise(realm, *writer.closed_promise(), error);
    }
    // 2. Otherwise, set writer.[[closedPromise]] to a promise rejected with error.
    else {
        writer.set_closed_promise(WebIDL::create_rejected_promise(realm, error));
    }

    // 3. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*writer.closed_promise());
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-ensure-ready-promise-rejected
void writable_stream_default_writer_ensure_ready_promise_rejected(WritableStreamDefaultWriter& writer, JS::Value error)
{
    auto& realm = writer.realm();

    // 1. If writer.[[readyPromise]].[[PromiseState]] is "pending", reject writer.[[readyPromise]] with error.
    auto& ready_promise = verify_cast<JS::Promise>(*writer.ready_promise()->promise());
    if (ready_promise.state() == JS::Promise::State::Pending) {
        WebIDL::reject_promise(realm, *writer.ready_promise(), error);
    }
    // 2. Otherwise, set writer.[[readyPromise]] to a promise rejected with error.
    else {
        writer.set_ready_promise(WebIDL::create_rejected_promise(realm, error));
    }

    // 3. Set writer.[[readyPromise]].[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*writer.ready_promise());
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-get-desired-size
Optional<double> writable_stream_default_writer_get_desired_size(WritableStreamDefaultWriter const& writer)
{
    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Let state be stream.[[state]].
    auto state = stream->state();

    // 3. If state is "errored" or "erroring", return null.
    if (state == WritableStream::State::Errored || state == WritableStream::State::Erroring)
        return {};

    // 4. If state is "closed", return 0.
    if (state == WritableStream::State::Closed)
        return 0.0;

    // 5. Return ! WritableStreamDefaultControllerGetDesiredSize(stream.[[controller]]).
    return writable_stream_default_controller_get_desired_size(*stream->controller());
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-release
void writable_stream_default_writer_release(WritableStreamDefaultWriter& writer)
{
    auto& realm = writer.realm();

    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Assert: stream.[[writer]] is writer.
    VERIFY(stream->writer().ptr() == &writer);

    // 4. Let releasedError be a new TypeError.
    auto released_error = JS::TypeError::create(realm, "Writer's stream lock has been released"sv);

    // 5. Perform ! WritableStreamDefaultWriterEnsureReadyPromiseRejected(writer, releasedError).
    writable_stream_default_writer_ensure_ready_promise_rejected(writer, released_error);

    // 6. Perform ! WritableStreamDefaultWriterEnsureClosedPromiseRejected(writer, releasedError).
    writable_stream_default_writer_ensure_closed_promise_rejected(writer, released_error);

    // 7. Set stream.[[writer]] to undefined.
    stream->set_writer({});

    // 8. Set writer.[[stream]] to undefined.
    writer.set_stream({});
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-write
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_default_writer_write(WritableStreamDefaultWriter& writer, JS::Value chunk)
{
    auto& realm = writer.realm();

    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Let controller be stream.[[controller]].
    auto controller = stream->controller();

    // 4. Let chunkSize be ! WritableStreamDefaultControllerGetChunkSize(controller, chunk).
    auto chunk_size = writable_stream_default_controller_get_chunk_size(*controller, chunk);

    // 5. If stream is not equal to writer.[[stream]], return a promise rejected with a TypeError exception.
    if (stream.ptr() != writer.stream().ptr()) {
        auto exception = JS::TypeError::create(realm, "Writer's locked stream changed during write"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 6. Let state be stream.[[state]].
    auto state = stream->state();

    // 7. If state is "errored", return a promise rejected with stream.[[storedError]].
    if (state == WritableStream::State::Errored)
        return WebIDL::create_rejected_promise(realm, stream->stored_error());

    // 8. If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state is "closed", return a promise rejected with a TypeError exception indicating that the stream is closing or closed.
    if (writable_stream_close_queued_or_in_flight(*stream) || state == WritableStream::State::Closed) {
        auto exception = JS::TypeError::create(realm, "Cannot write to a writer whose stream is closing or already closed"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 9. If state is "erroring", return a promise rejected with stream.[[storedError]].
    if (state == WritableStream::State::Erroring)
        return WebIDL::create_rejected_promise(realm, stream->stored_error());

    // 10. Assert: state is "writable".
    VERIFY(state == WritableStream::State::Writable);

    // 11. Let promise be ! WritableStreamAddWriteRequest(stream).
    auto promise = writable_stream_add_write_request(*stream);

    // 12. Perform ! WritableStreamDefaultControllerWrite(controller, chunk, chunkSize).
    writable_stream_default_controller_write(*controller, chunk, chunk_size);

    // 13. Return promise.
    return promise;
}

// https://streams.spec.whatwg.org/#set-up-writable-stream-default-controller
WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller(WritableStream& stream, WritableStreamDefaultController& controller, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<WriteAlgorithm> write_algorithm, JS::NonnullGCPtr<CloseAlgorithm> close_algorithm, JS::NonnullGCPtr<AbortAlgorithm> abort_algorithm, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Assert: stream implements WritableStream.

    // 2. Assert: stream.[[controller]] is undefined.
    VERIFY(!stream.controller());

    // 3. Set controller.[[stream]] to stream.
    controller.set_stream(stream);

    // 4. Set stream.[[controller]] to controller.
    stream.set_controller(controller);

    // 5. Perform ! ResetQueue(controller).
    reset_queue(controller);

    // 6. Set controller.[[signal]] to a new AbortSignal.
    controller.set_signal(realm.heap().allocate<DOM::AbortSignal>(realm, realm));

    // 7. Set controller.[[started]] to false.
    controller.set_started(false);

    // 8. Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm.
    controller.set_strategy_size_algorithm(size_algorithm);

    // 9. Set controller.[[strategyHWM]] to highWaterMark.
    controller.set_strategy_hwm(high_water_mark);

    // 10. Set controller.[[writeAlgorithm]] to writeAlgorithm.
    controller.set_write_algorithm(write_algorithm);

    // 11. Set controller.[[closeAlgorithm]] to closeAlgorithm.
    controller.set_close_algorithm(close_algorithm);

    // 12. Set controller.[[abortAlgorithm]] to abortAlgorithm.
    controller.set_abort_algorithm(abort_algorithm);

    // 13. Let backpressure be ! WritableStreamDefaultControllerGetBackpressure(controller).
    auto backpressure = writable_stream_default_controller_get_backpressure(controller);

    // 14. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
    writable_stream_update_backpressure(stream, backpressure);

    // 15. Let startResult be the result of performing startAlgorithm. (This may throw an exception.)
    auto start_result = TRY(start_algorithm->function()());

    // 16. Let startPromise be a promise resolved with startResult.
    auto start_promise = WebIDL::create_resolved_promise(realm, start_result);

    // 17. Upon fulfillment of startPromise,
    WebIDL::upon_fulfillment(*start_promise, JS::create_heap_function(realm.heap(), [&controller, &stream](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Assert: stream.[[state]] is "writable" or "erroring".
        auto state = stream.state();
        VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

        // 2. Set controller.[[started]] to true.
        controller.set_started(true);

        // 3. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
        writable_stream_default_controller_advance_queue_if_needed(controller);

        return JS::js_undefined();
    }));

    // 18. Upon rejection of startPromise with reason r,
    WebIDL::upon_rejection(*start_promise, JS::create_heap_function(realm.heap(), [&stream, &controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Assert: stream.[[state]] is "writable" or "erroring".
        auto state = stream.state();
        VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

        // 2. Set controller.[[started]] to true.
        controller.set_started(true);

        // 3. Perform ! WritableStreamDealWithRejection(stream, r).
        writable_stream_deal_with_rejection(stream, reason);

        return JS::js_undefined();
    }));

    return {};
}

// https://streams.spec.whatwg.org/#set-up-writable-stream-default-controller-from-underlying-sink
WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller_from_underlying_sink(WritableStream& stream, JS::Value underlying_sink_value, UnderlyingSink& underlying_sink, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Let controller be a new WritableStreamDefaultController.
    auto controller = realm.heap().allocate<WritableStreamDefaultController>(realm, realm);

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = JS::create_heap_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); });

    // 3. Let writeAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto write_algorithm = JS::create_heap_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let closeAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto close_algorithm = JS::create_heap_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. Let abortAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto abort_algorithm = JS::create_heap_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 6. If underlyingSinkDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of invoking underlyingSinkDict["start"] with argument list « controller » and callback this value underlyingSink.
    if (underlying_sink.start) {
        start_algorithm = JS::create_heap_function(realm.heap(), [controller, underlying_sink_value, callback = underlying_sink.start]() -> WebIDL::ExceptionOr<JS::Value> {
            // Note: callback does not return a promise, so invoke_callback may return an abrupt completion
            return TRY(WebIDL::invoke_callback(*callback, underlying_sink_value, controller)).release_value();
        });
    }

    // 7. If underlyingSinkDict["write"] exists, then set writeAlgorithm to an algorithm which takes an argument chunk and returns the result of invoking underlyingSinkDict["write"] with argument list « chunk, controller » and callback this value underlyingSink.
    if (underlying_sink.write) {
        write_algorithm = JS::create_heap_function(realm.heap(), [&realm, controller, underlying_sink_value, callback = underlying_sink.write](JS::Value chunk) {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, underlying_sink_value, chunk, controller)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 8. If underlyingSinkDict["close"] exists, then set closeAlgorithm to an algorithm which returns the result of invoking underlyingSinkDict["close"] with argument list «» and callback this value underlyingSink.
    if (underlying_sink.close) {
        close_algorithm = JS::create_heap_function(realm.heap(), [&realm, underlying_sink_value, callback = underlying_sink.close]() {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, underlying_sink_value)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 9. If underlyingSinkDict["abort"] exists, then set abortAlgorithm to an algorithm which takes an argument reason and returns the result of invoking underlyingSinkDict["abort"] with argument list « reason » and callback this value underlyingSink.
    if (underlying_sink.abort) {
        abort_algorithm = JS::create_heap_function(realm.heap(), [&realm, underlying_sink_value, callback = underlying_sink.abort](JS::Value reason) {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, underlying_sink_value, reason)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 10. Perform ? SetUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm).
    TRY(set_up_writable_stream_default_controller(stream, controller, start_algorithm, write_algorithm, close_algorithm, abort_algorithm, high_water_mark, size_algorithm));

    return {};
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-advance-queue-if-needed
void writable_stream_default_controller_advance_queue_if_needed(WritableStreamDefaultController& controller)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If controller.[[started]] is false, return.
    if (!controller.started())
        return;

    // 3. If stream.[[inFlightWriteRequest]] is not undefined, return.
    if (stream->in_flight_write_request())
        return;

    // 4. Let state be stream.[[state]].
    auto state = stream->state();

    // 5. Assert: state is not "closed" or "errored".
    VERIFY(state != WritableStream::State::Closed && state != WritableStream::State::Errored);

    // 6. If state is "erroring",
    if (state == WritableStream::State::Erroring) {
        // 1. Perform ! WritableStreamFinishErroring(stream).
        writable_stream_finish_erroring(*stream);

        // 2. Return.
        return;
    }

    // 7. If controller.[[queue]] is empty, return.
    if (controller.queue().is_empty())
        return;

    // 8. Let value be ! PeekQueueValue(controller).
    auto value = peek_queue_value(controller);

    // 9. If value is the close sentinel, perform ! WritableStreamDefaultControllerProcessClose(controller).
    if (is_close_sentinel(value)) {
        writable_stream_default_controller_process_close(controller);
    }
    // 10. Otherwise, perform ! WritableStreamDefaultControllerProcessWrite(controller, value).
    else {
        writable_stream_default_controller_process_write(controller, value);
    }
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-clear-algorithms
void writable_stream_default_controller_clear_algorithms(WritableStreamDefaultController& controller)
{
    // 1. Set controller.[[writeAlgorithm]] to undefined.
    controller.set_write_algorithm({});

    // 2. Set controller.[[closeAlgorithm]] to undefined.
    controller.set_close_algorithm({});

    // 3. Set controller.[[abortAlgorithm]] to undefined.
    controller.set_abort_algorithm({});

    // 4. Set controller.[[strategySizeAlgorithm]] to undefined.
    controller.set_strategy_size_algorithm({});
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-close
void writable_stream_default_controller_close(WritableStreamDefaultController& controller)
{
    // 1. Perform ! EnqueueValueWithSize(controller, close sentinel, 0).
    MUST(enqueue_value_with_size(controller, create_close_sentinel(), JS::Value(0.0)));

    // 2. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
    writable_stream_default_controller_advance_queue_if_needed(controller);
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-error
void writable_stream_default_controller_error(WritableStreamDefaultController& controller, JS::Value error)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Assert: stream.[[state]] is "writable".
    VERIFY(stream->state() == WritableStream::State::Writable);

    // 3. Perform ! WritableStreamDefaultControllerClearAlgorithms(controller).
    writable_stream_default_controller_clear_algorithms(controller);

    // 4. Perform ! WritableStreamStartErroring(stream, error).
    writable_stream_start_erroring(stream, error);
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-error-if-needed
void writable_stream_default_controller_error_if_needed(WritableStreamDefaultController& controller, JS::Value error)
{
    // 1. If controller.[[stream]].[[state]] is "writable", perform ! WritableStreamDefaultControllerError(controller, error).
    if (controller.stream()->state() == WritableStream::State::Writable)
        writable_stream_default_controller_error(controller, error);
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-get-backpressure
bool writable_stream_default_controller_get_backpressure(WritableStreamDefaultController const& controller)
{
    // 1. Let desiredSize be ! WritableStreamDefaultControllerGetDesiredSize(controller).
    auto desired_size = writable_stream_default_controller_get_desired_size(controller);

    // 2. Return true if desiredSize ≤ 0, or false otherwise.
    return desired_size <= 0.0;
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-get-chunk-size
JS::Value writable_stream_default_controller_get_chunk_size(WritableStreamDefaultController& controller, JS::Value chunk)
{
    // 1. Let returnValue be the result of performing controller.[[strategySizeAlgorithm]], passing in chunk, and interpreting the result as a completion record.
    auto return_value = controller.strategy_size_algorithm()->function()(chunk);

    // 2. If returnValue is an abrupt completion,
    if (return_value.is_abrupt()) {
        // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, returnValue.[[Value]]).
        writable_stream_default_controller_error_if_needed(controller, *return_value.release_value());

        // 2. Return 1.
        return JS::Value { 1.0 };
    }

    // 3. Return returnValue.[[Value]].
    return *return_value.release_value();
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-get-desired-size
double writable_stream_default_controller_get_desired_size(WritableStreamDefaultController const& controller)
{
    // 1. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
    return controller.strategy_hwm() - controller.queue_total_size();
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-process-close
void writable_stream_default_controller_process_close(WritableStreamDefaultController& controller)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Perform ! WritableStreamMarkCloseRequestInFlight(stream).
    writable_stream_mark_close_request_in_flight(*stream);

    // 3. Perform ! DequeueValue(controller).
    dequeue_value(controller);

    // 4. Assert: controller.[[queue]] is empty.
    VERIFY(controller.queue().is_empty());

    // 5. Let sinkClosePromise be the result of performing controller.[[closeAlgorithm]].
    auto sink_close_promise = controller.close_algorithm()->function()();

    // 6. Perform ! WritableStreamDefaultControllerClearAlgorithms(controller).
    writable_stream_default_controller_clear_algorithms(controller);

    // 7. Upon fulfillment of sinkClosePromise,
    WebIDL::upon_fulfillment(*sink_close_promise, JS::create_heap_function(controller.heap(), [stream](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform ! WritableStreamFinishInFlightClose(stream).
        writable_stream_finish_in_flight_close(*stream);

        return JS::js_undefined();
    }));

    // 8. Upon rejection of sinkClosePromise with reason reason,
    WebIDL::upon_rejection(*sink_close_promise, JS::create_heap_function(controller.heap(), [stream = stream](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform ! WritableStreamFinishInFlightCloseWithError(stream, reason).
        writable_stream_finish_in_flight_close_with_error(*stream, reason);

        return JS::js_undefined();
    }));
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-process-write
void writable_stream_default_controller_process_write(WritableStreamDefaultController& controller, JS::Value chunk)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Perform ! WritableStreamMarkFirstWriteRequestInFlight(stream).
    writable_stream_mark_first_write_request_in_flight(*stream);

    // 3. Let sinkWritePromise be the result of performing controller.[[writeAlgorithm]], passing in chunk.
    auto sink_write_promise = controller.write_algorithm()->function()(chunk);

    // 4. Upon fulfillment of sinkWritePromise,
    WebIDL::upon_fulfillment(*sink_write_promise, JS::create_heap_function(controller.heap(), [&controller, stream](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Perform ! WritableStreamFinishInFlightWrite(stream).
        writable_stream_finish_in_flight_write(*stream);

        // 2. Let state be stream.[[state]].
        auto state = stream->state();

        // 3. Assert: state is "writable" or "erroring".
        VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

        // 4. Perform ! DequeueValue(controller).
        dequeue_value(controller);

        // 5. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and state is "writable",
        if (!writable_stream_close_queued_or_in_flight(*stream) && state == WritableStream::State::Writable) {
            // 1. Let backpressure be ! WritableStreamDefaultControllerGetBackpressure(controller).
            auto backpressure = writable_stream_default_controller_get_backpressure(controller);

            // 2. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
            writable_stream_update_backpressure(*stream, backpressure);
        }

        // 6 .Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
        writable_stream_default_controller_advance_queue_if_needed(controller);

        return JS::js_undefined();
    }));

    // 5. Upon rejection of sinkWritePromise with reason,
    WebIDL::upon_rejection(*sink_write_promise, JS::create_heap_function(controller.heap(), [&controller, stream](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. If stream.[[state]] is "writable", perform ! WritableStreamDefaultControllerClearAlgorithms(controller).
        if (stream->state() == WritableStream::State::Writable)
            writable_stream_default_controller_clear_algorithms(controller);

        // 2. Perform ! WritableStreamFinishInFlightWriteWithError(stream, reason).
        writable_stream_finish_in_flight_write_with_error(*stream, reason);

        return JS::js_undefined();
    }));
}

// https://streams.spec.whatwg.org/#writable-stream-default-controller-write
void writable_stream_default_controller_write(WritableStreamDefaultController& controller, JS::Value chunk, JS::Value chunk_size)
{
    auto& vm = controller.vm();

    // 1. Let enqueueResult be EnqueueValueWithSize(controller, chunk, chunkSize).
    auto enqueue_result = enqueue_value_with_size(controller, chunk, chunk_size);

    // 2. If enqueueResult is an abrupt completion,
    if (enqueue_result.is_exception()) {
        auto throw_completion = Bindings::throw_dom_exception_if_needed(vm, [&] { return enqueue_result; }).throw_completion();

        // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, enqueueResult.[[Value]]).
        writable_stream_default_controller_error_if_needed(controller, *throw_completion.release_value());

        // 2. Return.
        return;
    }

    // 3. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 4. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and stream.[[state]] is "writable",
    if (!writable_stream_close_queued_or_in_flight(*stream) && stream->state() == WritableStream::State::Writable) {
        // 1. Let backpressure be ! WritableStreamDefaultControllerGetBackpressure(controller).
        auto backpressure = writable_stream_default_controller_get_backpressure(controller);

        // 2. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
        writable_stream_update_backpressure(*stream, backpressure);
    }

    // 5. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
    writable_stream_default_controller_advance_queue_if_needed(controller);
}

// https://streams.spec.whatwg.org/#initialize-transform-stream
void initialize_transform_stream(TransformStream& stream, JS::NonnullGCPtr<JS::PromiseCapability> start_promise, double writable_high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> writable_size_algorithm, double readable_high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> readable_size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Let startAlgorithm be an algorithm that returns startPromise.
    auto writable_start_algorithm = JS::create_heap_function(realm.heap(), [start_promise]() -> WebIDL::ExceptionOr<JS::Value> {
        return start_promise->promise();
    });

    auto readable_start_algorithm = JS::create_heap_function(realm.heap(), [start_promise]() -> WebIDL::ExceptionOr<JS::Value> {
        return start_promise->promise();
    });

    // 2. Let writeAlgorithm be the following steps, taking a chunk argument:
    auto write_algorithm = JS::create_heap_function(realm.heap(), [&stream](JS::Value chunk) {
        // 1. Return ! TransformStreamDefaultSinkWriteAlgorithm(stream, chunk).
        return transform_stream_default_sink_write_algorithm(stream, chunk);
    });

    // 3. Let abortAlgorithm be the following steps, taking a reason argument:
    auto abort_algorithm = JS::create_heap_function(realm.heap(), [&stream](JS::Value reason) {
        // 1. Return ! TransformStreamDefaultSinkAbortAlgorithm(stream, reason).
        return transform_stream_default_sink_abort_algorithm(stream, reason);
    });

    // 4. Let closeAlgorithm be the following steps:
    auto close_algorithm = JS::create_heap_function(realm.heap(), [&stream]() {
        // 1. Return ! TransformStreamDefaultSinkCloseAlgorithm(stream).
        return transform_stream_default_sink_close_algorithm(stream);
    });

    // 5. Set stream.[[writable]] to ! CreateWritableStream(startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, writableHighWaterMark, writableSizeAlgorithm).
    stream.set_writable(MUST(create_writable_stream(realm, writable_start_algorithm, write_algorithm, close_algorithm, abort_algorithm, writable_high_water_mark, writable_size_algorithm)));

    // 6. Let pullAlgorithm be the following steps:
    auto pull_algorithm = JS::create_heap_function(realm.heap(), [&stream]() {
        // 1. Return ! TransformStreamDefaultSourcePullAlgorithm(stream).
        return transform_stream_default_source_pull_algorithm(stream);
    });

    // 7. Let cancelAlgorithm be the following steps, taking a reason argument:
    auto cancel_algorithm = JS::create_heap_function(realm.heap(), [&stream](JS::Value reason) {
        // 1. Return ! TransformStreamDefaultSourceCancelAlgorithm(stream, reason).
        return transform_stream_default_source_cancel_algorithm(stream, reason);
    });

    // 8. Set stream.[[readable]] to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, readableHighWaterMark, readableSizeAlgorithm).
    stream.set_readable(MUST(create_readable_stream(realm, readable_start_algorithm, pull_algorithm, cancel_algorithm, readable_high_water_mark, readable_size_algorithm)));

    // 9. Set stream.[[backpressure]] and stream.[[backpressureChangePromise]] to undefined.
    stream.set_backpressure({});
    stream.set_backpressure_change_promise({});

    // 10. Perform ! TransformStreamSetBackpressure(stream, true).
    transform_stream_set_backpressure(stream, true);

    // 11. Set stream.[[controller]] to undefined.
    stream.set_controller({});
}

// https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller
void set_up_transform_stream_default_controller(TransformStream& stream, TransformStreamDefaultController& controller, JS::NonnullGCPtr<TransformAlgorithm> transform_algorithm, JS::NonnullGCPtr<FlushAlgorithm> flush_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm)
{
    // 1. Assert: stream implements TransformStream.
    // 2. Assert: stream.[[controller]] is undefined.
    VERIFY(!stream.controller());

    // 3. Set controller.[[stream]] to stream.
    controller.set_stream(stream);

    // 4. Set stream.[[controller]] to controller.
    stream.set_controller(controller);

    // 5. Set controller.[[transformAlgorithm]] to transformAlgorithm.
    controller.set_transform_algorithm(transform_algorithm);

    // 6. Set controller.[[flushAlgorithm]] to flushAlgorithm.
    controller.set_flush_algorithm(flush_algorithm);

    // 7. Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
    controller.set_cancel_algorithm(cancel_algorithm);
}

// https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller-from-transformer
void set_up_transform_stream_default_controller_from_transformer(TransformStream& stream, JS::Value transformer, Transformer& transformer_dict)
{
    auto& realm = stream.realm();
    auto& vm = realm.vm();

    // 1. Let controller be a new TransformStreamDefaultController.
    auto controller = realm.heap().allocate<TransformStreamDefaultController>(realm, realm);

    // 2. Let transformAlgorithm be the following steps, taking a chunk argument:
    auto transform_algorithm = JS::create_heap_function(realm.heap(), [controller, &realm, &vm](JS::Value chunk) {
        // 1. Let result be TransformStreamDefaultControllerEnqueue(controller, chunk).
        auto result = transform_stream_default_controller_enqueue(*controller, chunk);

        // 2. If result is an abrupt completion, return a promise rejected with result.[[Value]].
        if (result.is_error()) {
            auto throw_completion = Bindings::dom_exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, *throw_completion.release_value());
        }

        // 3. Otherwise, return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 3. Let flushAlgorithm be an algorithm which returns a promise resolved with undefined.
    auto flush_algorithm = JS::create_heap_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let cancelAlgorithm be an algorithm which returns a promise resolved with undefined.
    auto cancel_algorithm = JS::create_heap_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. If transformerDict["transform"] exists, set transformAlgorithm to an algorithm which takes an argument chunk
    //    and returns the result of invoking transformerDict["transform"] with argument list « chunk, controller » and
    //    callback this value transformer.
    if (transformer_dict.transform) {
        transform_algorithm = JS::create_heap_function(realm.heap(), [controller, &realm, transformer, callback = transformer_dict.transform](JS::Value chunk) {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, transformer, chunk, controller)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 6. If transformerDict["flush"] exists, set flushAlgorithm to an algorithm which returns the result of invoking
    //    transformerDict["flush"] with argument list « controller » and callback this value transformer.
    if (transformer_dict.flush) {
        flush_algorithm = JS::create_heap_function(realm.heap(), [&realm, transformer, callback = transformer_dict.flush, controller]() {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, transformer, controller)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 7. If transformerDict["cancel"] exists, set cancelAlgorithm to an algorithm which takes an argument reason and returns
    // the result of invoking transformerDict["cancel"] with argument list « reason » and callback this value transformer.
    if (transformer_dict.cancel) {
        cancel_algorithm = JS::create_heap_function(realm.heap(), [&realm, transformer, callback = transformer_dict.cancel](JS::Value reason) {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, transformer, reason)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 8. Perform ! SetUpTransformStreamDefaultController(stream, controller, transformAlgorithm, flushAlgorithm).
    set_up_transform_stream_default_controller(stream, *controller, transform_algorithm, flush_algorithm, cancel_algorithm);
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-clear-algorithms
void transform_stream_default_controller_clear_algorithms(TransformStreamDefaultController& controller)
{
    // NOTE: This is observable using weak references. See tc39/proposal-weakrefs#31 for more detail.
    // 1. Set controller.[[transformAlgorithm]] to undefined.
    controller.set_transform_algorithm({});

    // 2. Set controller.[[flushAlgorithm]] to undefined.
    controller.set_flush_algorithm({});

    // 3. Set controller.[[cancelAlgorithm]] to undefined.
    controller.set_cancel_algorithm({});
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-enqueue
WebIDL::ExceptionOr<void> transform_stream_default_controller_enqueue(TransformStreamDefaultController& controller, JS::Value chunk)
{
    auto& vm = controller.vm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Let readableController be stream.[[readable]].[[controller]].
    VERIFY(stream->readable()->controller().has_value() && stream->readable()->controller()->has<JS::NonnullGCPtr<ReadableStreamDefaultController>>());
    auto& readable_controller = stream->readable()->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();

    // 3. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(readableController) is false, throw a TypeError exception.
    if (!readable_stream_default_controller_can_close_or_enqueue(readable_controller))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "ReadableController is either closed or not readable."sv };

    // 4. Let enqueueResult be ReadableStreamDefaultControllerEnqueue(readableController, chunk).
    auto enqueue_result = readable_stream_default_controller_enqueue(readable_controller, chunk);

    // 5. If enqueueResult is an abrupt completion,
    if (enqueue_result.is_error()) {
        auto throw_completion = Bindings::dom_exception_to_throw_completion(vm, enqueue_result.exception());

        // 1. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, enqueueResult.[[Value]]).
        transform_stream_error_writable_and_unblock_write(*stream, throw_completion.value().value());

        // 2. Throw stream.[[readable]].[[storedError]].
        return JS::throw_completion(stream->readable()->stored_error());
    }

    // 6. Let backpressure be ! ReadableStreamDefaultControllerHasBackpressure(readableController).
    auto backpressure = readable_stream_default_controller_has_backpressure(readable_controller);

    // 7. If backpressure is not stream.[[backpressure]],
    if (backpressure != stream->backpressure()) {
        // 1. Assert: backpressure is true.
        VERIFY(backpressure);

        // 2. Perform ! TransformStreamSetBackpressure(stream, true).
        transform_stream_set_backpressure(*stream, true);
    }

    return {};
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-error
void transform_stream_default_controller_error(TransformStreamDefaultController& controller, JS::Value error)
{
    // 1. Perform ! TransformStreamError(controller.[[stream]], e).
    transform_stream_error(*controller.stream(), error);
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-terminate
void transform_stream_default_controller_terminate(TransformStreamDefaultController& controller)
{
    auto& realm = controller.realm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Let readableController be stream.[[readable]].[[controller]].
    VERIFY(stream->readable()->controller().has_value() && stream->readable()->controller()->has<JS::NonnullGCPtr<ReadableStreamDefaultController>>());
    auto readable_controller = stream->readable()->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();

    // 3. Perform ! ReadableStreamDefaultControllerClose(readableController).
    readable_stream_default_controller_close(readable_controller);

    // 4. Let error be a TypeError exception indicating that the stream has been terminated.
    auto error = JS::TypeError::create(realm, "Stream has been terminated."sv);

    // 5. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, error).
    transform_stream_error_writable_and_unblock_write(*stream, error);
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-perform-transform
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_controller_perform_transform(TransformStreamDefaultController& controller, JS::Value chunk)
{
    auto& realm = controller.realm();

    // 1. Let transformPromise be the result of performing controller.[[transformAlgorithm]], passing chunk.
    auto transform_promise = controller.transform_algorithm()->function()(chunk);

    // 2. Return the result of reacting to transformPromise with the following rejection steps given the argument r:
    auto react_result = WebIDL::react_to_promise(*transform_promise,
        {},
        JS::create_heap_function(realm.heap(), [&controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! TransformStreamError(controller.[[stream]], r).
            transform_stream_error(*controller.stream(), reason);

            // 2. Throw r.
            return JS::throw_completion(reason);
        }));

    return WebIDL::create_resolved_promise(realm, react_result);
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-abort-algorithm
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_sink_abort_algorithm(TransformStream& stream, JS::Value reason)
{
    auto& realm = stream.realm();

    // 1. Let controller be stream.[[controller]].
    auto controller = stream.controller();
    VERIFY(controller);

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if (controller->finish_promise())
        return JS::NonnullGCPtr { *controller->finish_promise() };

    // 3. Let readable be stream.[[readable]].
    auto readable = stream.readable();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller->set_finish_promise(WebIDL::create_promise(realm));

    // 5. Let cancelPromise be the result of performing controller.[[cancelAlgorithm]], passing reason.
    auto cancel_promise = controller->cancel_algorithm()->function()(reason);

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(*controller);

    // 7. React to cancelPromise:
    WebIDL::react_to_promise(
        *cancel_promise,
        // 1. If cancelPromise was fulfilled, then:
        JS::create_heap_function(realm.heap(), [&realm, readable, controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. If readable.[[state]] is "errored", reject controller.[[finishPromise]] with readable.[[storedError]].
            if (readable->state() == ReadableStream::State::Errored) {
                WebIDL::reject_promise(realm, *controller->finish_promise(), readable->stored_error());
            }
            // 2. Otherwise:
            else {
                VERIFY(readable->controller().has_value() && readable->controller()->has<JS::NonnullGCPtr<ReadableStreamDefaultController>>());
                // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], reason).
                readable_stream_default_controller_error(readable->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>(), reason);

                // 2. Resolve controller.[[finishPromise]] with undefined.
                WebIDL::resolve_promise(realm, *controller->finish_promise(), JS::js_undefined());
            }
            return JS::js_undefined();
        }),
        // 2. If cancelPromise was rejected with reason r, then:
        JS::create_heap_function(realm.heap(), [&realm, readable, controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            VERIFY(readable->controller().has_value() && readable->controller()->has<JS::NonnullGCPtr<ReadableStreamDefaultController>>());
            // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], r).
            readable_stream_default_controller_error(readable->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>(), reason);

            // 2. Reject controller.[[finishPromise]] with r.
            WebIDL::reject_promise(realm, *controller->finish_promise(), reason);

            return JS::js_undefined();
        }));

    // 8. Return controller.[[finishPromise]].
    return JS::NonnullGCPtr { *controller->finish_promise() };
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-close-algorithm
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_sink_close_algorithm(TransformStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let readable be stream.[[readable]].
    auto readable = stream.readable();

    // 2. Let controller be stream.[[controller]].
    auto controller = stream.controller();

    // 3. Let flushPromise be the result of performing controller.[[flushAlgorithm]].
    auto flush_promise = controller->flush_algorithm()->function()();

    // 4. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(*controller);

    // 5. Return the result of reacting to flushPromise:
    auto react_result = WebIDL::react_to_promise(
        *flush_promise,
        // 1. If flushPromise was fulfilled, then:
        JS::create_heap_function(realm.heap(), [readable](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. If readable.[[state]] is "errored", throw readable.[[storedError]].
            if (readable->state() == ReadableStream::State::Errored)
                return JS::throw_completion(readable->stored_error());

            VERIFY(readable->controller().has_value() && readable->controller()->has<JS::NonnullGCPtr<ReadableStreamDefaultController>>());
            // 2. Perform ! ReadableStreamDefaultControllerClose(readable.[[controller]]).
            readable_stream_default_controller_close(readable->controller().value().get<JS::NonnullGCPtr<ReadableStreamDefaultController>>());

            return JS::js_undefined();
        }),
        // 2. If flushPromise was rejected with reason r, then:
        JS::create_heap_function(realm.heap(), [&stream, readable](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! TransformStreamError(stream, r).
            transform_stream_error(stream, reason);

            // 2. Throw readable.[[storedError]].
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, readable->stored_error().as_string().utf8_string() };
        }));

    return WebIDL::create_resolved_promise(realm, react_result);
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-write-algorithm
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_sink_write_algorithm(TransformStream& stream, JS::Value chunk)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[writable]].[[state]] is "writable".
    VERIFY(stream.writable()->state() == WritableStream::State::Writable);

    // 2. Let controller be stream.[[controller]].
    auto controller = stream.controller();

    // 3. If stream.[[backpressure]] is true,
    if (stream.backpressure().has_value() && *stream.backpressure()) {
        // 1. Let backpressureChangePromise be stream.[[backpressureChangePromise]].
        auto backpressure_change_promise = stream.backpressure_change_promise();

        // 2. Assert: backpressureChangePromise is not undefined.
        VERIFY(backpressure_change_promise);

        // 3. Return the result of reacting to backpressureChangePromise with the following fulfillment steps:
        auto react_result = WebIDL::react_to_promise(*backpressure_change_promise,
            JS::create_heap_function(realm.heap(), [&stream, controller, chunk](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                // 1. Let writable be stream.[[writable]].
                auto writable = stream.writable();

                // 2. Let state be writable.[[state]].
                auto state = writable->state();

                // 3. If state is "erroring", throw writable.[[storedError]].
                if (state == WritableStream::State::Erroring)
                    return JS::throw_completion(writable->stored_error());

                // 4. Assert: state is "writable".
                VERIFY(state == WritableStream::State::Writable);

                // 5. Return ! TransformStreamDefaultControllerPerformTransform(controller, chunk).
                return transform_stream_default_controller_perform_transform(*controller, chunk)->promise();
            }),
            {});

        return WebIDL::create_resolved_promise(realm, react_result);
    }

    // 4. Return ! TransformStreamDefaultControllerPerformTransform(controller, chunk).
    return transform_stream_default_controller_perform_transform(*controller, chunk);
}

JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_source_pull_algorithm(TransformStream& stream)
{
    // 1. Assert: stream.[[backpressure]] is true.
    VERIFY(stream.backpressure().has_value() && *stream.backpressure());

    // 2. Assert: stream.[[backpressureChangePromise]] is not undefined.
    VERIFY(stream.backpressure_change_promise());

    // 3. Perform ! TransformStreamSetBackpressure(stream, false).
    transform_stream_set_backpressure(stream, false);

    // 4. Return stream.[[backpressureChangePromise]].
    return JS::NonnullGCPtr { *stream.backpressure_change_promise() };
}

// https://streams.spec.whatwg.org/#transform-stream-default-source-cancel
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_source_cancel_algorithm(TransformStream& stream, JS::Value reason)
{
    auto& realm = stream.realm();

    // 1. Let controller be stream.[[controller]].
    auto controller = stream.controller();

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if (controller->finish_promise())
        return JS::NonnullGCPtr { *controller->finish_promise() };

    // 3. Let writable be stream.[[writable]].
    auto writable = stream.writable();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller->set_finish_promise(WebIDL::create_promise(realm));

    // 5. Let cancelPromise be the result of performing controller.[[cancelAlgorithm]], passing reason.
    auto cancel_promise = controller->cancel_algorithm()->function()(reason);

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(*controller);

    // 7. React to cancelPromise:
    WebIDL::react_to_promise(
        *cancel_promise,
        // 1. If cancelPromise was fulfilled, then:
        JS::create_heap_function(realm.heap(), [&realm, writable, controller, &stream](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. If writable.[[state]] is "errored", reject controller.[[finishPromise]] with writable.[[storedError]].
            if (writable->state() == WritableStream::State::Errored) {
                WebIDL::reject_promise(realm, *controller->finish_promise(), writable->stored_error());
            }
            // 2. Otherwise:
            else {
                // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(writable.[[controller]], reason).
                writable_stream_default_controller_error_if_needed(*writable->controller(), reason);
                // 2. Perform ! TransformStreamUnblockWrite(stream).
                transform_stream_unblock_write(stream);
                // 3. Resolve controller.[[finishPromise]] with undefined.
                WebIDL::resolve_promise(realm, *controller->finish_promise(), JS::js_undefined());
            }
            return JS::js_undefined();
        }),
        // 2. If cancelPromise was rejected with reason r, then:
        JS::create_heap_function(realm.heap(), [&realm, writable, &stream, controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(writable.[[controller]], r).
            writable_stream_default_controller_error_if_needed(*writable->controller(), reason);
            // 2. Perform ! TransformStreamUnblockWrite(stream).
            transform_stream_unblock_write(stream);
            // 3. Reject controller.[[finishPromise]] with r.
            WebIDL::reject_promise(realm, *controller->finish_promise(), reason);
            return JS::js_undefined();
        }));

    // 8. Return controller.[[finishPromise]].
    return JS::NonnullGCPtr { *controller->finish_promise() };
}

// https://streams.spec.whatwg.org/#transform-stream-error
void transform_stream_error(TransformStream& stream, JS::Value error)
{
    VERIFY(stream.readable()->controller().has_value() && stream.readable()->controller()->has<JS::NonnullGCPtr<ReadableStreamDefaultController>>());

    auto readable_controller = stream.readable()->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();

    // 1. Perform ! ReadableStreamDefaultControllerError(stream.[[readable]].[[controller]], e).
    readable_stream_default_controller_error(*readable_controller, error);

    // 2. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, e).
    transform_stream_error_writable_and_unblock_write(stream, error);
}

// https://streams.spec.whatwg.org/#transform-stream-error-writable-and-unblock-write
void transform_stream_error_writable_and_unblock_write(TransformStream& stream, JS::Value error)
{
    // 1. Perform ! TransformStreamDefaultControllerClearAlgorithms(stream.[[controller]]).
    transform_stream_default_controller_clear_algorithms(*stream.controller());

    // 2. Perform ! WritableStreamDefaultControllerErrorIfNeeded(stream.[[writable]].[[controller]], e).
    writable_stream_default_controller_error_if_needed(*stream.writable()->controller(), error);

    // 3. If stream.[[backpressure]] is true, perform ! TransformStreamSetBackpressure(stream, false).
    if (stream.backpressure().has_value() && *stream.backpressure())
        transform_stream_set_backpressure(stream, false);
}

//  https://streams.spec.whatwg.org/#transform-stream-set-backpressure
void transform_stream_set_backpressure(TransformStream& stream, bool backpressure)
{
    auto& realm = stream.realm();

    // 1. Assert: stream.[[backpressure]] is not backpressure.
    VERIFY(stream.backpressure() != backpressure);

    // 2. If stream.[[backpressureChangePromise]] is not undefined, resolve stream.[[backpressureChangePromise]] with undefined.
    if (stream.backpressure_change_promise())
        WebIDL::resolve_promise(realm, *stream.backpressure_change_promise(), JS::js_undefined());

    // 3. Set stream.[[backpressureChangePromise]] to a new promise.
    stream.set_backpressure_change_promise(WebIDL::create_promise(realm));

    // 4. Set stream.[[backpressure]] to backpressure.
    stream.set_backpressure(backpressure);
}

// https://streams.spec.whatwg.org/#transformstream-set-up
void transform_stream_set_up(TransformStream& stream, JS::NonnullGCPtr<TransformAlgorithm> transform_algorithm, JS::GCPtr<FlushAlgorithm> flush_algorithm, JS::GCPtr<CancelAlgorithm> cancel_algorithm)
{
    auto& realm = stream.realm();

    // 1. Let writableHighWaterMark be 1.
    auto writable_high_water_mark = 1.0;

    // 2. Let writableSizeAlgorithm be an algorithm that returns 1.
    auto writable_size_algorithm = JS::create_heap_function(realm.heap(), [](JS::Value) {
        return JS::normal_completion(JS::Value { 1 });
    });

    // 3. Let readableHighWaterMark be 0.
    auto readable_high_water_mark = 0.0;

    // 4. Let readableSizeAlgorithm be an algorithm that returns 1.
    auto readable_size_algorithm = JS::create_heap_function(realm.heap(), [](JS::Value) {
        return JS::normal_completion(JS::Value { 1 });
    });

    // 5. Let transformAlgorithmWrapper be an algorithm that runs these steps given a value chunk:
    auto transform_algorithm_wrapper = JS::create_heap_function(realm.heap(), [&realm, transform_algorithm](JS::Value chunk) -> JS::NonnullGCPtr<WebIDL::Promise> {
        // 1. Let result be the result of running transformAlgorithm given chunk. If this throws an exception e, return a promise rejected with e.
        JS::GCPtr<JS::PromiseCapability> result = nullptr;
        result = transform_algorithm->function()(chunk);

        // 2. If result is a Promise, then return result.
        if (result)
            return JS::NonnullGCPtr { *result };

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 6. Let flushAlgorithmWrapper be an algorithm that runs these steps:
    auto flush_algorithm_wrapper = JS::create_heap_function(realm.heap(), [&realm, flush_algorithm]() -> JS::NonnullGCPtr<WebIDL::Promise> {
        // 1. Let result be the result of running flushAlgorithm, if flushAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        JS::GCPtr<JS::PromiseCapability> result = nullptr;
        if (flush_algorithm)
            result = flush_algorithm->function()();

        // 2. If result is a Promise, then return result.
        if (result)
            return JS::NonnullGCPtr { *result };

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 7. Let cancelAlgorithmWrapper be an algorithm that runs these steps given a value reason:
    auto cancel_algorithm_wrapper = JS::create_heap_function(realm.heap(), [&realm, cancel_algorithm](JS::Value reason) -> JS::NonnullGCPtr<WebIDL::Promise> {
        // 1. Let result be the result of running cancelAlgorithm given reason, if cancelAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        JS::GCPtr<JS::PromiseCapability> result = nullptr;
        if (cancel_algorithm)
            result = cancel_algorithm->function()(reason);

        // 2. If result is a Promise, then return result.
        if (result)
            return JS::NonnullGCPtr { *result };

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 8. Let startPromise be a promise resolved with undefined.
    auto start_promise = WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 9. Perform ! InitializeTransformStream(stream, startPromise, writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark, readableSizeAlgorithm).
    initialize_transform_stream(stream, start_promise, writable_high_water_mark, writable_size_algorithm, readable_high_water_mark, readable_size_algorithm);

    // 10. Let controller be a new TransformStreamDefaultController.
    auto controller = realm.heap().allocate<TransformStreamDefaultController>(realm, realm);

    // 11. Perform ! SetUpTransformStreamDefaultController(stream, controller, transformAlgorithmWrapper, flushAlgorithmWrapper, cancelAlgorithmWrapper).
    set_up_transform_stream_default_controller(stream, controller, transform_algorithm_wrapper, flush_algorithm_wrapper, cancel_algorithm_wrapper);
}

// https://streams.spec.whatwg.org/#transform-stream-unblock-write
void transform_stream_unblock_write(TransformStream& stream)
{
    // 1. If stream.[[backpressure]] is true, perform ! TransformStreamSetBackpressure(stream, false).
    if (stream.backpressure().has_value() && stream.backpressure().value())
        transform_stream_set_backpressure(stream, false);
}

// https://streams.spec.whatwg.org/#is-non-negative-number
bool is_non_negative_number(JS::Value value)
{
    // 1. If Type(v) is not Number, return false.
    if (!value.is_number())
        return false;

    // 2. If v is NaN, return false.
    if (value.is_nan())
        return false;

    // 3. If v < 0, return false.
    if (value.as_double() < 0.0)
        return false;

    // 4. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#can-transfer-array-buffer
bool can_transfer_array_buffer(JS::ArrayBuffer const& array_buffer)
{
    // 1. Assert: Type(O) is Object.
    // 2. Assert: O has an [[ArrayBufferData]] internal slot.

    // 3. If ! IsDetachedBuffer(O) is true, return false.
    if (array_buffer.is_detached())
        return false;

    // 4. If SameValue(O.[[ArrayBufferDetachKey]], undefined) is false, return false.
    if (!JS::same_value(array_buffer.detach_key(), JS::js_undefined()))
        return false;

    // 5. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#abstract-opdef-cloneasuint8array
WebIDL::ExceptionOr<JS::Value> clone_as_uint8_array(JS::Realm& realm, WebIDL::ArrayBufferView& view)
{
    auto& vm = realm.vm();

    // 1. Assert: Type(O) is Object.
    // 2. Assert: O has an [[ViewedArrayBuffer]] internal slot.

    // 3. Assert: ! IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is false.
    VERIFY(!view.viewed_array_buffer()->is_detached());

    // 4. Let buffer be ? CloneArrayBuffer(O.[[ViewedArrayBuffer]], O.[[ByteOffset]], O.[[ByteLength]], %ArrayBuffer%).
    auto* buffer = TRY(JS::clone_array_buffer(vm, *view.viewed_array_buffer(), view.byte_offset(), view.byte_length()));

    // 5. Let array be ! Construct(%Uint8Array%, « buffer »).
    auto array = MUST(JS::construct(vm, *realm.intrinsics().uint8_array_constructor(), buffer));

    // 5. Return array.
    return array;
}

// https://streams.spec.whatwg.org/#abstract-opdef-structuredclone
WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Realm& realm, JS::Value value)
{
    auto& vm = realm.vm();

    // 1. Let serialized be ? StructuredSerialize(v).
    auto serialized = TRY(HTML::structured_serialize(vm, value));

    // 2. Return ? StructuredDeserialize(serialized, the current Realm).
    return TRY(HTML::structured_deserialize(vm, serialized, realm, {}));
}

// https://streams.spec.whatwg.org/#close-sentinel
// Non-standard function that implements the "close sentinel" value.
JS::Value create_close_sentinel()
{
    // The close sentinel is a unique value enqueued into [[queue]], in lieu of a chunk, to signal that the stream is closed. It is only used internally, and is never exposed to web developers.
    // Note: We use the empty Value to signal this as, similarly to the note above, the empty value is not exposed to nor creatable by web developers.
    return {};
}

// https://streams.spec.whatwg.org/#close-sentinel
// Non-standard function that implements the "If value is a close sentinel" check.
bool is_close_sentinel(JS::Value value)
{
    return value.is_empty();
}

// Non-standard function to aid in converting a user-provided function into a WebIDL::Callback. This is essentially
// what the Bindings generator would do at compile time, but at runtime instead.
JS::ThrowCompletionOr<JS::Handle<WebIDL::CallbackType>> property_to_callback(JS::VM& vm, JS::Value value, JS::PropertyKey const& property_key, WebIDL::OperationReturnsPromise operation_returns_promise)
{
    auto property = TRY(value.get(vm, property_key));

    if (property.is_undefined())
        return JS::Handle<WebIDL::CallbackType> {};

    if (!property.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, property.to_string_without_side_effects());

    return vm.heap().allocate_without_realm<WebIDL::CallbackType>(property.as_object(), HTML::incumbent_settings_object(), operation_returns_promise);
}

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller-from-underlying-source
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller_from_underlying_source(ReadableStream& stream, JS::Value underlying_source, UnderlyingSource const& underlying_source_dict, double high_water_mark)
{
    auto& realm = stream.realm();

    // 1. Let controller be a new ReadableByteStreamController.
    auto controller = stream.heap().allocate<ReadableByteStreamController>(realm, realm);

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = JS::create_heap_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); });

    // 3. Let pullAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto pull_algorithm = JS::create_heap_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let cancelAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto cancel_algorithm = JS::create_heap_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. If underlyingSourceDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of invoking underlyingSourceDict["start"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source_dict.start) {
        start_algorithm = JS::create_heap_function(realm.heap(), [controller, underlying_source, callback = underlying_source_dict.start]() -> WebIDL::ExceptionOr<JS::Value> {
            // Note: callback does not return a promise, so invoke_callback may return an abrupt completion
            return TRY(WebIDL::invoke_callback(*callback, underlying_source, controller)).release_value();
        });
    }

    // 6. If underlyingSourceDict["pull"] exists, then set pullAlgorithm to an algorithm which returns the result of invoking underlyingSourceDict["pull"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source_dict.pull) {
        pull_algorithm = JS::create_heap_function(realm.heap(), [&realm, controller, underlying_source, callback = underlying_source_dict.pull]() {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, underlying_source, controller)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 7. If underlyingSourceDict["cancel"] exists, then set cancelAlgorithm to an algorithm which takes an argument reason and returns the result of invoking underlyingSourceDict["cancel"] with argument list « reason » and callback this value underlyingSource.
    if (underlying_source_dict.cancel) {
        cancel_algorithm = JS::create_heap_function(realm.heap(), [&realm, underlying_source, callback = underlying_source_dict.cancel](JS::Value reason) {
            // Note: callback returns a promise, so invoke_callback will never return an abrupt completion
            auto result = MUST(WebIDL::invoke_callback(*callback, underlying_source, reason)).release_value();
            return WebIDL::create_resolved_promise(realm, result);
        });
    }

    // 8. Let autoAllocateChunkSize be underlyingSourceDict["autoAllocateChunkSize"], if it exists, or undefined otherwise.
    auto auto_allocate_chunk_size = underlying_source_dict.auto_allocate_chunk_size.has_value()
        ? JS::Value(underlying_source_dict.auto_allocate_chunk_size.value())
        : JS::js_undefined();

    // 9. If autoAllocateChunkSize is 0, then throw a TypeError exception.
    if (auto_allocate_chunk_size.is_integral_number() && auto_allocate_chunk_size.as_double() == 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot use an auto allocate chunk size of 0"sv };

    // 10. Perform ? SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, autoAllocateChunkSize).
    return set_up_readable_byte_stream_controller(stream, controller, start_algorithm, pull_algorithm, cancel_algorithm, high_water_mark, auto_allocate_chunk_size);
}

}
