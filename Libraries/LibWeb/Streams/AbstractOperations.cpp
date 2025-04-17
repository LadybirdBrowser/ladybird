/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>
#include <LibWeb/Streams/Transformer.h>
#include <LibWeb/Streams/UnderlyingSink.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultController.h>
#include <LibWeb/Streams/WritableStreamDefaultWriter.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#close-sentinel
// Non-standard function that implements the "close sentinel" value.
static JS::Value create_close_sentinel()
{
    // The close sentinel is a unique value enqueued into [[queue]], in lieu of a chunk, to signal that the stream is closed. It is only used internally, and is never exposed to web developers.
    // Note: We use the empty Value to signal this as, similarly to the note above, the empty value is not exposed to nor creatable by web developers.
    return JS::js_special_empty_value();
}

// https://streams.spec.whatwg.org/#close-sentinel
// Non-standard function that implements the "If value is a close sentinel" check.
static bool is_close_sentinel(JS::Value value)
{
    return value.is_special_empty_value();
}

// https://streams.spec.whatwg.org/#make-size-algorithm-from-size-function
GC::Ref<SizeAlgorithm> extract_size_algorithm(JS::VM& vm, QueuingStrategy const& strategy)
{
    // 1. If strategy["size"] does not exist, return an algorithm that returns 1.
    if (!strategy.size)
        return GC::create_function(vm.heap(), [](JS::Value) { return JS::normal_completion(JS::Value(1)); });

    // 2. Return an algorithm that performs the following steps, taking a chunk argument:
    return GC::create_function(vm.heap(), [size = strategy.size](JS::Value chunk) {
        // 1. Return the result of invoking strategy["size"] with argument list « chunk ».
        return WebIDL::invoke_callback(*size, {}, { { chunk } });
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

// https://streams.spec.whatwg.org/#create-writable-stream
WebIDL::ExceptionOr<GC::Ref<WritableStream>> create_writable_stream(JS::Realm& realm, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<WriteAlgorithm> write_algorithm, GC::Ref<CloseAlgorithm> close_algorithm, GC::Ref<AbortAlgorithm> abort_algorithm, double high_water_mark, GC::Ref<SizeAlgorithm> size_algorithm)
{
    // 1. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
    VERIFY(is_non_negative_number(JS::Value { high_water_mark }));

    // 2. Let stream be a new WritableStream.
    auto stream = realm.create<WritableStream>(realm);

    // 3. Perform ! InitializeWritableStream(stream).
    initialize_writable_stream(*stream);

    // 4. Let controller be a new WritableStreamDefaultController.
    auto controller = realm.create<WritableStreamDefaultController>(realm);

    // 5. Perform ? SetUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm).
    TRY(set_up_writable_stream_default_controller(*stream, *controller, move(start_algorithm), move(write_algorithm), move(close_algorithm), move(abort_algorithm), high_water_mark, move(size_algorithm)));

    // 6. Return stream.
    return stream;
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
WebIDL::ExceptionOr<GC::Ref<WritableStreamDefaultWriter>> acquire_writable_stream_default_writer(WritableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let writer be a new WritableStreamDefaultWriter.
    auto writer = realm.create<WritableStreamDefaultWriter>(realm);

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
    // FIXME: Exactly when we should effectively be using the relevant realm of `this` is to be clarified by the spec.
    //        For now, we do so as needed by WPT tests. See: https://github.com/whatwg/streams/issues/1213
    auto& realm = HTML::relevant_realm(writer);

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

// https://streams.spec.whatwg.org/#transfer-array-buffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> transfer_array_buffer(JS::Realm& realm, JS::ArrayBuffer& buffer)
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

// https://streams.spec.whatwg.org/#writable-stream-abort
GC::Ref<WebIDL::Promise> writable_stream_abort(WritableStream& stream, JS::Value reason)
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
GC::Ref<WebIDL::Promise> writable_stream_close(WritableStream& stream)
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
GC::Ref<WebIDL::Promise> writable_stream_add_write_request(WritableStream& stream)
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

    WebIDL::react_to_promise(promise,
        // 13. Upon fulfillment of promise,
        GC::create_function(realm.heap(), [&realm, &stream, abort_promise = abort_request.promise](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Resolve abortRequest’s promise with undefined.
            WebIDL::resolve_promise(realm, abort_promise, JS::js_undefined());

            // 2. Perform ! WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
            writable_stream_reject_close_and_closed_promise_if_needed(stream);

            return JS::js_undefined();
        }),

        // 14. Upon rejection of promise with reason reason,
        GC::create_function(realm.heap(), [&realm, &stream, abort_promise = abort_request.promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
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
GC::Ref<WebIDL::Promise> writable_stream_default_writer_abort(WritableStreamDefaultWriter& writer, JS::Value reason)
{
    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Return ! WritableStreamAbort(stream, reason).
    return writable_stream_abort(*stream, reason);
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-close
GC::Ref<WebIDL::Promise> writable_stream_default_writer_close(WritableStreamDefaultWriter& writer)
{
    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Return ! WritableStreamClose(stream).
    return writable_stream_close(*stream);
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-close-with-error-propagation
GC::Ref<WebIDL::Promise> writable_stream_default_writer_close_with_error_propagation(WritableStreamDefaultWriter& writer)
{
    auto& realm = writer.realm();

    // 1. Let stream be writer.[[stream]].
    auto stream = writer.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Let state be stream.[[state]].
    auto state = stream->state();

    // 4. If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state is "closed", return a promise resolved with undefined.
    if (writable_stream_close_queued_or_in_flight(*stream) || state == WritableStream::State::Closed)
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 5. If state is "errored", return a promise rejected with stream.[[storedError]].
    if (state == WritableStream::State::Errored)
        return WebIDL::create_rejected_promise(realm, stream->stored_error());

    // 6. Assert: state is "writable" or "erroring".
    VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

    // 7. Return ! WritableStreamDefaultWriterClose(writer).
    return writable_stream_default_writer_close(writer);
}

// https://streams.spec.whatwg.org/#writable-stream-default-writer-ensure-closed-promise-rejected
void writable_stream_default_writer_ensure_closed_promise_rejected(WritableStreamDefaultWriter& writer, JS::Value error)
{
    auto& realm = writer.realm();

    // 1. If writer.[[closedPromise]].[[PromiseState]] is "pending", reject writer.[[closedPromise]] with error.
    auto& closed_promise = as<JS::Promise>(*writer.closed_promise()->promise());
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
    auto& ready_promise = as<JS::Promise>(*writer.ready_promise()->promise());
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
GC::Ref<WebIDL::Promise> writable_stream_default_writer_write(WritableStreamDefaultWriter& writer, JS::Value chunk)
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
WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller(WritableStream& stream, WritableStreamDefaultController& controller, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<WriteAlgorithm> write_algorithm, GC::Ref<CloseAlgorithm> close_algorithm, GC::Ref<AbortAlgorithm> abort_algorithm, double high_water_mark, GC::Ref<SizeAlgorithm> size_algorithm)
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
    controller.set_signal(realm.create<DOM::AbortSignal>(realm));

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

    WebIDL::react_to_promise(start_promise,
        // 17. Upon fulfillment of startPromise,
        GC::create_function(realm.heap(), [&controller, &stream](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Assert: stream.[[state]] is "writable" or "erroring".
            auto state = stream.state();
            VERIFY(state == WritableStream::State::Writable || state == WritableStream::State::Erroring);

            // 2. Set controller.[[started]] to true.
            controller.set_started(true);

            // 3. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(controller).
            writable_stream_default_controller_advance_queue_if_needed(controller);

            return JS::js_undefined();
        }),

        // 18. Upon rejection of startPromise with reason r,
        GC::create_function(realm.heap(), [&stream, &controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
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
WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller_from_underlying_sink(WritableStream& stream, JS::Value underlying_sink_value, UnderlyingSink& underlying_sink, double high_water_mark, GC::Ref<SizeAlgorithm> size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Let controller be a new WritableStreamDefaultController.
    auto controller = realm.create<WritableStreamDefaultController>(realm);

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); });

    // 3. Let writeAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto write_algorithm = GC::create_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let closeAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto close_algorithm = GC::create_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. Let abortAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto abort_algorithm = GC::create_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 6. If underlyingSinkDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSinkDict["start"] with argument list « controller », exception behavior "rethrow", and
    //    callback this value underlyingSink.
    if (underlying_sink.start) {
        start_algorithm = GC::create_function(realm.heap(), [controller, underlying_sink_value, callback = underlying_sink.start]() -> WebIDL::ExceptionOr<JS::Value> {
            return TRY(WebIDL::invoke_callback(*callback, underlying_sink_value, WebIDL::ExceptionBehavior::Rethrow, { { controller } }));
        });
    }

    // 7. If underlyingSinkDict["write"] exists, then set writeAlgorithm to an algorithm which takes an argument chunk
    //    and returns the result of invoking underlyingSinkDict["write"] with argument list « chunk, controller » and
    //    callback this value underlyingSink.
    if (underlying_sink.write) {
        write_algorithm = GC::create_function(realm.heap(), [controller, underlying_sink_value, callback = underlying_sink.write](JS::Value chunk) {
            return WebIDL::invoke_promise_callback(*callback, underlying_sink_value, { { chunk, controller } });
        });
    }

    // 8. If underlyingSinkDict["close"] exists, then set closeAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSinkDict["close"] with argument list «» and callback this value underlyingSink.
    if (underlying_sink.close) {
        close_algorithm = GC::create_function(realm.heap(), [underlying_sink_value, callback = underlying_sink.close]() {
            return WebIDL::invoke_promise_callback(*callback, underlying_sink_value, {});
        });
    }

    // 9. If underlyingSinkDict["abort"] exists, then set abortAlgorithm to an algorithm which takes an argument reason
    //    and returns the result of invoking underlyingSinkDict["abort"] with argument list « reason » and callback this
    //    value underlyingSink.
    if (underlying_sink.abort) {
        abort_algorithm = GC::create_function(realm.heap(), [underlying_sink_value, callback = underlying_sink.abort](JS::Value reason) {
            return WebIDL::invoke_promise_callback(*callback, underlying_sink_value, { { reason } });
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
    // 1. If controller.[[strategySizeAlgorithm]] is undefined, then:
    if (!controller.strategy_size_algorithm()) {
        // 1. Assert: controller.[[stream]].[[state]] is not "writable".
        VERIFY(controller.stream()->state() != WritableStream::State::Writable);

        // 2. Return 1.
        return JS::Value { 1.0 };
    }

    // 2. Let returnValue be the result of performing controller.[[strategySizeAlgorithm]], passing in chunk, and interpreting the result as a completion record.
    auto return_value = controller.strategy_size_algorithm()->function()(chunk);

    // 3. If returnValue is an abrupt completion,
    if (return_value.is_abrupt()) {
        // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, returnValue.[[Value]]).
        writable_stream_default_controller_error_if_needed(controller, return_value.release_value());

        // 2. Return 1.
        return JS::Value { 1.0 };
    }

    // 4. Return returnValue.[[Value]].
    return return_value.release_value();
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

    WebIDL::react_to_promise(sink_close_promise,
        // 7. Upon fulfillment of sinkClosePromise,
        GC::create_function(controller.heap(), [stream](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! WritableStreamFinishInFlightClose(stream).
            writable_stream_finish_in_flight_close(*stream);

            return JS::js_undefined();
        }),

        // 8. Upon rejection of sinkClosePromise with reason reason,
        GC::create_function(controller.heap(), [stream = stream](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
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

    WebIDL::react_to_promise(sink_write_promise,
        // 4. Upon fulfillment of sinkWritePromise,
        GC::create_function(controller.heap(), [&controller, stream](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
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
        }),

        // 5. Upon rejection of sinkWritePromise with reason,
        GC::create_function(controller.heap(), [&controller, stream](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
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
        writable_stream_default_controller_error_if_needed(controller, throw_completion.release_value());

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
void initialize_transform_stream(TransformStream& stream, GC::Ref<WebIDL::Promise> start_promise, double writable_high_water_mark, GC::Ref<SizeAlgorithm> writable_size_algorithm, double readable_high_water_mark, GC::Ref<SizeAlgorithm> readable_size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Let startAlgorithm be an algorithm that returns startPromise.
    auto writable_start_algorithm = GC::create_function(realm.heap(), [start_promise]() -> WebIDL::ExceptionOr<JS::Value> {
        return start_promise->promise();
    });

    auto readable_start_algorithm = GC::create_function(realm.heap(), [start_promise]() -> WebIDL::ExceptionOr<JS::Value> {
        return start_promise->promise();
    });

    // 2. Let writeAlgorithm be the following steps, taking a chunk argument:
    auto write_algorithm = GC::create_function(realm.heap(), [&stream](JS::Value chunk) {
        // 1. Return ! TransformStreamDefaultSinkWriteAlgorithm(stream, chunk).
        return transform_stream_default_sink_write_algorithm(stream, chunk);
    });

    // 3. Let abortAlgorithm be the following steps, taking a reason argument:
    auto abort_algorithm = GC::create_function(realm.heap(), [&stream](JS::Value reason) {
        // 1. Return ! TransformStreamDefaultSinkAbortAlgorithm(stream, reason).
        return transform_stream_default_sink_abort_algorithm(stream, reason);
    });

    // 4. Let closeAlgorithm be the following steps:
    auto close_algorithm = GC::create_function(realm.heap(), [&stream]() {
        // 1. Return ! TransformStreamDefaultSinkCloseAlgorithm(stream).
        return transform_stream_default_sink_close_algorithm(stream);
    });

    // 5. Set stream.[[writable]] to ! CreateWritableStream(startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, writableHighWaterMark, writableSizeAlgorithm).
    stream.set_writable(MUST(create_writable_stream(realm, writable_start_algorithm, write_algorithm, close_algorithm, abort_algorithm, writable_high_water_mark, writable_size_algorithm)));

    // 6. Let pullAlgorithm be the following steps:
    auto pull_algorithm = GC::create_function(realm.heap(), [&stream]() {
        // 1. Return ! TransformStreamDefaultSourcePullAlgorithm(stream).
        return transform_stream_default_source_pull_algorithm(stream);
    });

    // 7. Let cancelAlgorithm be the following steps, taking a reason argument:
    auto cancel_algorithm = GC::create_function(realm.heap(), [&stream](JS::Value reason) {
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
void set_up_transform_stream_default_controller(TransformStream& stream, TransformStreamDefaultController& controller, GC::Ref<TransformAlgorithm> transform_algorithm, GC::Ref<FlushAlgorithm> flush_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm)
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
    auto controller = realm.create<TransformStreamDefaultController>(realm);

    // 2. Let transformAlgorithm be the following steps, taking a chunk argument:
    auto transform_algorithm = GC::create_function(realm.heap(), [controller, &realm, &vm](JS::Value chunk) {
        // 1. Let result be TransformStreamDefaultControllerEnqueue(controller, chunk).
        auto result = transform_stream_default_controller_enqueue(*controller, chunk);

        // 2. If result is an abrupt completion, return a promise rejected with result.[[Value]].
        if (result.is_error()) {
            auto throw_completion = Bindings::exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, throw_completion.release_value());
        }

        // 3. Otherwise, return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 3. Let flushAlgorithm be an algorithm which returns a promise resolved with undefined.
    auto flush_algorithm = GC::create_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let cancelAlgorithm be an algorithm which returns a promise resolved with undefined.
    auto cancel_algorithm = GC::create_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. If transformerDict["transform"] exists, set transformAlgorithm to an algorithm which takes an argument chunk
    //    and returns the result of invoking transformerDict["transform"] with argument list « chunk, controller » and
    //    callback this value transformer.
    if (transformer_dict.transform) {
        transform_algorithm = GC::create_function(realm.heap(), [controller, transformer, callback = transformer_dict.transform](JS::Value chunk) {
            return WebIDL::invoke_promise_callback(*callback, transformer, { { chunk, controller } });
        });
    }

    // 6. If transformerDict["flush"] exists, set flushAlgorithm to an algorithm which returns the result of invoking
    //    transformerDict["flush"] with argument list « controller » and callback this value transformer.
    if (transformer_dict.flush) {
        flush_algorithm = GC::create_function(realm.heap(), [transformer, callback = transformer_dict.flush, controller]() {
            return WebIDL::invoke_promise_callback(*callback, transformer, { { controller } });
        });
    }

    // 7. If transformerDict["cancel"] exists, set cancelAlgorithm to an algorithm which takes an argument reason and returns
    // the result of invoking transformerDict["cancel"] with argument list « reason » and callback this value transformer.
    if (transformer_dict.cancel) {
        cancel_algorithm = GC::create_function(realm.heap(), [transformer, callback = transformer_dict.cancel](JS::Value reason) {
            return WebIDL::invoke_promise_callback(*callback, transformer, { { reason } });
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
    VERIFY(stream->readable()->controller().has_value() && stream->readable()->controller()->has<GC::Ref<ReadableStreamDefaultController>>());
    auto& readable_controller = stream->readable()->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

    // 3. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(readableController) is false, throw a TypeError exception.
    if (!readable_stream_default_controller_can_close_or_enqueue(readable_controller))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "ReadableController is either closed or not readable."sv };

    // 4. Let enqueueResult be ReadableStreamDefaultControllerEnqueue(readableController, chunk).
    auto enqueue_result = readable_stream_default_controller_enqueue(readable_controller, chunk);

    // 5. If enqueueResult is an abrupt completion,
    if (enqueue_result.is_error()) {
        auto throw_completion = Bindings::exception_to_throw_completion(vm, enqueue_result.exception());

        // 1. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, enqueueResult.[[Value]]).
        transform_stream_error_writable_and_unblock_write(*stream, throw_completion.value());

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
    VERIFY(stream->readable()->controller().has_value() && stream->readable()->controller()->has<GC::Ref<ReadableStreamDefaultController>>());
    auto readable_controller = stream->readable()->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

    // 3. Perform ! ReadableStreamDefaultControllerClose(readableController).
    readable_stream_default_controller_close(readable_controller);

    // 4. Let error be a TypeError exception indicating that the stream has been terminated.
    auto error = JS::TypeError::create(realm, "Stream has been terminated."sv);

    // 5. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, error).
    transform_stream_error_writable_and_unblock_write(*stream, error);
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-perform-transform
GC::Ref<WebIDL::Promise> transform_stream_default_controller_perform_transform(TransformStreamDefaultController& controller, JS::Value chunk)
{
    auto& realm = controller.realm();

    // 1. Let transformPromise be the result of performing controller.[[transformAlgorithm]], passing chunk.
    auto transform_promise = controller.transform_algorithm()->function()(chunk);

    // 2. Return the result of reacting to transformPromise with the following rejection steps given the argument r:
    auto react_result = WebIDL::react_to_promise(*transform_promise,
        {},
        GC::create_function(realm.heap(), [&controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! TransformStreamError(controller.[[stream]], r).
            transform_stream_error(*controller.stream(), reason);

            // 2. Throw r.
            return JS::throw_completion(reason);
        }));

    return react_result;
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-abort-algorithm
GC::Ref<WebIDL::Promise> transform_stream_default_sink_abort_algorithm(TransformStream& stream, JS::Value reason)
{
    auto& realm = stream.realm();

    // 1. Let controller be stream.[[controller]].
    auto controller = stream.controller();
    VERIFY(controller);

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if (controller->finish_promise())
        return *controller->finish_promise();

    // 3. Let readable be stream.[[readable]].
    auto readable = stream.readable();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller->set_finish_promise(WebIDL::create_promise(realm));

    // 5. Let cancelPromise be the result of performing controller.[[cancelAlgorithm]], passing reason.
    auto cancel_promise = controller->cancel_algorithm()->function()(reason);

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(*controller);

    // 7. React to cancelPromise:
    WebIDL::react_to_promise(cancel_promise,
        // 1. If cancelPromise was fulfilled, then:
        GC::create_function(realm.heap(), [&realm, readable, controller, reason](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. If readable.[[state]] is "errored", reject controller.[[finishPromise]] with readable.[[storedError]].
            if (readable->state() == ReadableStream::State::Errored) {
                WebIDL::reject_promise(realm, *controller->finish_promise(), readable->stored_error());
            }
            // 2. Otherwise:
            else {
                VERIFY(readable->controller().has_value() && readable->controller()->has<GC::Ref<ReadableStreamDefaultController>>());
                // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], reason).
                readable_stream_default_controller_error(readable->controller()->get<GC::Ref<ReadableStreamDefaultController>>(), reason);

                // 2. Resolve controller.[[finishPromise]] with undefined.
                WebIDL::resolve_promise(realm, *controller->finish_promise(), JS::js_undefined());
            }

            return JS::js_undefined();
        }),

        // 2. If cancelPromise was rejected with reason r, then:
        GC::create_function(realm.heap(), [&realm, readable, controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            VERIFY(readable->controller().has_value() && readable->controller()->has<GC::Ref<ReadableStreamDefaultController>>());
            // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], r).
            readable_stream_default_controller_error(readable->controller()->get<GC::Ref<ReadableStreamDefaultController>>(), reason);

            // 2. Reject controller.[[finishPromise]] with r.
            WebIDL::reject_promise(realm, *controller->finish_promise(), reason);

            return JS::js_undefined();
        }));

    // 8. Return controller.[[finishPromise]].
    return *controller->finish_promise();
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-close-algorithm
GC::Ref<WebIDL::Promise> transform_stream_default_sink_close_algorithm(TransformStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let controller be stream.[[controller]].
    auto controller = stream.controller();

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if (controller->finish_promise())
        return *controller->finish_promise();

    // 3. Let readable be stream.[[readable]].
    auto readable = stream.readable();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller->set_finish_promise(WebIDL::create_promise(realm));

    // 5. Let flushPromise be the result of performing controller.[[flushAlgorithm]].
    auto flush_promise = controller->flush_algorithm()->function()();

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(*controller);

    // 7. React to flushPromise:
    WebIDL::react_to_promise(flush_promise,
        // 1. If flushPromise was fulfilled, then:
        GC::create_function(realm.heap(), [&realm, controller, readable](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. If readable.[[state]] is "errored", reject controller.[[finishPromise]] with readable.[[storedError]].
            if (readable->state() == ReadableStream::State::Errored) {
                WebIDL::reject_promise(realm, *controller->finish_promise(), readable->stored_error());
            }
            // 2. Otherwise:
            else {
                // 1. Perform ! ReadableStreamDefaultControllerClose(readable.[[controller]]).
                readable_stream_default_controller_close(readable->controller().value().get<GC::Ref<ReadableStreamDefaultController>>());

                // 2. Resolve controller.[[finishPromise]] with undefined.
                WebIDL::resolve_promise(realm, *controller->finish_promise(), JS::js_undefined());
            }

            return JS::js_undefined();
        }),

        // 2. If flushPromise was rejected with reason r, then:
        GC::create_function(realm.heap(), [&realm, controller, readable](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], r).
            readable_stream_default_controller_error(readable->controller().value().get<GC::Ref<ReadableStreamDefaultController>>(), reason);

            // 2. Reject controller.[[finishPromise]] with r.
            WebIDL::reject_promise(realm, *controller->finish_promise(), reason);

            return JS::js_undefined();
        }));

    // 8. Return controller.[[finishPromise]].
    return *controller->finish_promise();
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-write-algorithm
GC::Ref<WebIDL::Promise> transform_stream_default_sink_write_algorithm(TransformStream& stream, JS::Value chunk)
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
            GC::create_function(realm.heap(), [&stream, controller, chunk](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
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

        return react_result;
    }

    // 4. Return ! TransformStreamDefaultControllerPerformTransform(controller, chunk).
    return transform_stream_default_controller_perform_transform(*controller, chunk);
}

GC::Ref<WebIDL::Promise> transform_stream_default_source_pull_algorithm(TransformStream& stream)
{
    // 1. Assert: stream.[[backpressure]] is true.
    VERIFY(stream.backpressure().has_value() && *stream.backpressure());

    // 2. Assert: stream.[[backpressureChangePromise]] is not undefined.
    VERIFY(stream.backpressure_change_promise());

    // 3. Perform ! TransformStreamSetBackpressure(stream, false).
    transform_stream_set_backpressure(stream, false);

    // 4. Return stream.[[backpressureChangePromise]].
    return GC::Ref { *stream.backpressure_change_promise() };
}

// https://streams.spec.whatwg.org/#transform-stream-default-source-cancel
GC::Ref<WebIDL::Promise> transform_stream_default_source_cancel_algorithm(TransformStream& stream, JS::Value reason)
{
    auto& realm = stream.realm();

    // 1. Let controller be stream.[[controller]].
    auto controller = stream.controller();

    // 2. If controller.[[finishPromise]] is not undefined, return controller.[[finishPromise]].
    if (controller->finish_promise())
        return *controller->finish_promise();

    // 3. Let writable be stream.[[writable]].
    auto writable = stream.writable();

    // 4. Let controller.[[finishPromise]] be a new promise.
    controller->set_finish_promise(WebIDL::create_promise(realm));

    // 5. Let cancelPromise be the result of performing controller.[[cancelAlgorithm]], passing reason.
    auto cancel_promise = controller->cancel_algorithm()->function()(reason);

    // 6. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    transform_stream_default_controller_clear_algorithms(*controller);

    // 7. React to cancelPromise:
    WebIDL::react_to_promise(cancel_promise,
        // 1. If cancelPromise was fulfilled, then:
        GC::create_function(realm.heap(), [&realm, writable, controller, &stream, reason](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
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
        GC::create_function(realm.heap(), [&realm, writable, &stream, controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(writable.[[controller]], r).
            writable_stream_default_controller_error_if_needed(*writable->controller(), reason);

            // 2. Perform ! TransformStreamUnblockWrite(stream).
            transform_stream_unblock_write(stream);

            // 3. Reject controller.[[finishPromise]] with r.
            WebIDL::reject_promise(realm, *controller->finish_promise(), reason);

            return JS::js_undefined();
        }));

    // 8. Return controller.[[finishPromise]].
    return *controller->finish_promise();
}

// https://streams.spec.whatwg.org/#transform-stream-error
void transform_stream_error(TransformStream& stream, JS::Value error)
{
    VERIFY(stream.readable()->controller().has_value() && stream.readable()->controller()->has<GC::Ref<ReadableStreamDefaultController>>());

    auto readable_controller = stream.readable()->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

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

    // 3. Perform ! TransformStreamUnblockWrite(stream).
    transform_stream_unblock_write(stream);
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
    // 1. If v is not a Number, return false.
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

// https://streams.spec.whatwg.org/#abstract-opdef-cancopydatablockbytes
bool can_copy_data_block_bytes_buffer(JS::ArrayBuffer const& to_buffer, u64 to_index, JS::ArrayBuffer const& from_buffer, u64 from_index, u64 count)
{
    // 1. Assert: toBuffer is an Object.
    // 2. Assert: toBuffer has an [[ArrayBufferData]] internal slot.
    // 3. Assert: fromBuffer is an Object.
    // 4. Assert: fromBuffer has an [[ArrayBufferData]] internal slot.

    // 5. If toBuffer is fromBuffer, return false.
    if (&to_buffer == &from_buffer)
        return false;

    // 6. If ! IsDetachedBuffer(toBuffer) is true, return false.
    if (to_buffer.is_detached())
        return false;

    // 7. If ! IsDetachedBuffer(fromBuffer) is true, return false.
    if (from_buffer.is_detached())
        return false;

    // 8. If toIndex + count > toBuffer.[[ArrayBufferByteLength]], return false.
    if (to_index + count > to_buffer.byte_length())
        return false;

    // 9. If fromIndex + count > fromBuffer.[[ArrayBufferByteLength]], return false.
    if (from_index + count > from_buffer.byte_length())
        return false;

    // 10. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#can-transfer-array-buffer
bool can_transfer_array_buffer(JS::ArrayBuffer const& array_buffer)
{
    // 1. Assert: O is an Object.
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

    // 1. Assert: O is an Object.
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
    return TRY(HTML::structured_deserialize(vm, serialized, realm));
}

}
