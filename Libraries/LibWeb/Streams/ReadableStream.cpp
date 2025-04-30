/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ReadableStreamPrototype.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamBYOBReader.h>
#include <LibWeb/Streams/ReadableStreamBYOBRequest.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/UnderlyingSource.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ReadableStream);

// https://streams.spec.whatwg.org/#rs-constructor
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> ReadableStream::construct_impl(JS::Realm& realm, Optional<GC::Root<JS::Object>> const& underlying_source_object, QueuingStrategy const& strategy)
{
    auto& vm = realm.vm();

    auto readable_stream = realm.create<ReadableStream>(realm);

    // 1. If underlyingSource is missing, set it to null.
    auto underlying_source = underlying_source_object.has_value() ? JS::Value(underlying_source_object.value()) : JS::js_null();

    // 2. Let underlyingSourceDict be underlyingSource, converted to an IDL value of type UnderlyingSource.
    auto underlying_source_dict = TRY(UnderlyingSource::from_value(vm, underlying_source));

    // 3. Perform ! InitializeReadableStream(this).

    // 4. If underlyingSourceDict["type"] is "bytes":
    if (underlying_source_dict.type.has_value() && underlying_source_dict.type.value() == ReadableStreamType::Bytes) {
        // 1. If strategy["size"] exists, throw a RangeError exception.
        if (strategy.size)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Size strategy not allowed for byte stream"sv };

        // 2. Let highWaterMark be ? ExtractHighWaterMark(strategy, 0).
        auto high_water_mark = TRY(extract_high_water_mark(strategy, 0));

        // 3. Perform ? SetUpReadableByteStreamControllerFromUnderlyingSource(this, underlyingSource, underlyingSourceDict, highWaterMark).
        TRY(set_up_readable_byte_stream_controller_from_underlying_source(*readable_stream, underlying_source, underlying_source_dict, high_water_mark));
    }
    // 5. Otherwise,
    else {
        // 1. Assert: underlyingSourceDict["type"] does not exist.
        VERIFY(!underlying_source_dict.type.has_value());

        // 2. Let sizeAlgorithm be ! ExtractSizeAlgorithm(strategy).
        auto size_algorithm = extract_size_algorithm(vm, strategy);

        // 3. Let highWaterMark be ? ExtractHighWaterMark(strategy, 1).
        auto high_water_mark = TRY(extract_high_water_mark(strategy, 1));

        // 4. Perform ? SetUpReadableStreamDefaultControllerFromUnderlyingSource(this, underlyingSource, underlyingSourceDict, highWaterMark, sizeAlgorithm).
        TRY(set_up_readable_stream_default_controller_from_underlying_source(*readable_stream, underlying_source, underlying_source_dict, high_water_mark, size_algorithm));
    }

    return readable_stream;
}

// https://streams.spec.whatwg.org/#rs-from
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> ReadableStream::from(JS::VM& vm, JS::Value async_iterable)
{
    // 1. Return ? ReadableStreamFromIterable(asyncIterable).
    return TRY(readable_stream_from_iterable(vm, async_iterable));
}

ReadableStream::ReadableStream(JS::Realm& realm)
    : PlatformObject(realm)
{
}

ReadableStream::~ReadableStream() = default;

// https://streams.spec.whatwg.org/#rs-locked
bool ReadableStream::locked() const
{
    // 1. Return ! IsReadableStreamLocked(this).
    return is_readable_stream_locked(*this);
}

// https://streams.spec.whatwg.org/#rs-cancel
GC::Ref<WebIDL::Promise> ReadableStream::cancel(JS::Value reason)
{
    auto& realm = this->realm();

    // 1. If ! IsReadableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
    if (is_readable_stream_locked(*this)) {
        auto exception = JS::TypeError::create(realm, "Cannot cancel a locked stream"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 2. Return ! ReadableStreamCancel(this, reason).
    return readable_stream_cancel(*this, reason);
}

// https://streams.spec.whatwg.org/#rs-get-reader
WebIDL::ExceptionOr<ReadableStreamReader> ReadableStream::get_reader(ReadableStreamGetReaderOptions const& options)
{
    // 1. If options["mode"] does not exist, return ? AcquireReadableStreamDefaultReader(this).
    if (!options.mode.has_value())
        return ReadableStreamReader { TRY(acquire_readable_stream_default_reader(*this)) };

    // 2. Assert: options["mode"] is "byob".
    VERIFY(*options.mode == Bindings::ReadableStreamReaderMode::Byob);

    // 3. Return ? AcquireReadableStreamBYOBReader(this).
    return ReadableStreamReader { TRY(acquire_readable_stream_byob_reader(*this)) };
}

// https://streams.spec.whatwg.org/#rs-pipe-through
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> ReadableStream::pipe_through(ReadableWritablePair transform, StreamPipeOptions const& options)
{
    // 1. If ! IsReadableStreamLocked(this) is true, throw a TypeError exception.
    if (is_readable_stream_locked(*this))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Failed to execute 'pipeThrough' on 'ReadableStream': Cannot pipe a locked stream"sv };

    // 2. If ! IsWritableStreamLocked(transform["writable"]) is true, throw a TypeError exception.
    if (is_writable_stream_locked(*transform.writable))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Failed to execute 'pipeThrough' on 'ReadableStream': parameter 1's 'writable' is locked"sv };

    // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
    auto signal = options.signal;

    // 4. Let promise be ! ReadableStreamPipeTo(this, transform["writable"], options["preventClose"], options["preventAbort"], options["preventCancel"], signal).
    auto promise = readable_stream_pipe_to(*this, *transform.writable, options.prevent_close, options.prevent_abort, options.prevent_cancel, signal);

    // 5. Set promise.[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*promise);

    // 6. Return transform["readable"].
    return GC::Ref { *transform.readable };
}

// https://streams.spec.whatwg.org/#rs-pipe-to
GC::Ref<WebIDL::Promise> ReadableStream::pipe_to(WritableStream& destination, StreamPipeOptions const& options)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. If ! IsReadableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
    if (is_readable_stream_locked(*this)) {
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("Failed to execute 'pipeTo' on 'ReadableStream': Cannot pipe a locked stream"sv));
    }

    // 2. If ! IsWritableStreamLocked(destination) is true, return a promise rejected with a TypeError exception.
    if (is_writable_stream_locked(destination)) {
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("Failed to execute 'pipeTo' on 'ReadableStream':  Cannot pipe to a locked stream"sv));
    }

    // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
    auto signal = options.signal;

    // 4. Return ! ReadableStreamPipeTo(this, destination, options["preventClose"], options["preventAbort"], options["preventCancel"], signal).
    return readable_stream_pipe_to(*this, destination, options.prevent_close, options.prevent_abort, options.prevent_cancel, signal);
}

// https://streams.spec.whatwg.org/#readablestream-tee
WebIDL::ExceptionOr<ReadableStreamPair> ReadableStream::tee(GC::Ptr<JS::Realm> target_realm)
{
    if (!target_realm)
        target_realm = &realm();

    // To tee a ReadableStream stream, return ? ReadableStreamTee(stream, true).
    return TRY(readable_stream_tee(*target_realm, *this, true));
}

// https://streams.spec.whatwg.org/#readablestream-close
void ReadableStream::close()
{
    controller()->visit(
        // 1. If stream.[[controller]] implements ReadableByteStreamController
        [&](GC::Ref<ReadableByteStreamController> controller) {
            // 1. Perform ! ReadableByteStreamControllerClose(stream.[[controller]]).
            MUST(readable_byte_stream_controller_close(controller));

            // 2. If stream.[[controller]].[[pendingPullIntos]] is not empty, perform ! ReadableByteStreamControllerRespond(stream.[[controller]], 0).
            if (!controller->pending_pull_intos().is_empty())
                MUST(readable_byte_stream_controller_respond(controller, 0));
        },

        // 2. Otherwise, perform ! ReadableStreamDefaultControllerClose(stream.[[controller]]).
        [&](GC::Ref<ReadableStreamDefaultController> controller) {
            readable_stream_default_controller_close(*controller);
        });
}

// https://streams.spec.whatwg.org/#readablestream-error
void ReadableStream::error(JS::Value error)
{
    controller()->visit(
        // 1. If stream.[[controller]] implements ReadableByteStreamController, then perform
        //    ! ReadableByteStreamControllerError(stream.[[controller]], e).
        [&](GC::Ref<ReadableByteStreamController> controller) {
            readable_byte_stream_controller_error(controller, error);
        },

        // 2. Otherwise, perform ! ReadableStreamDefaultControllerError(stream.[[controller]], e).
        [&](GC::Ref<ReadableStreamDefaultController> controller) {
            readable_stream_default_controller_error(controller, error);
        });
}

void ReadableStream::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ReadableStream);
    Base::initialize(realm);
}

void ReadableStream::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_controller.has_value())
        m_controller->visit([&](auto& controller) { visitor.visit(controller); });
    visitor.visit(m_stored_error);
    if (m_reader.has_value())
        m_reader->visit([&](auto& reader) { visitor.visit(reader); });
}

// https://streams.spec.whatwg.org/#readablestream-locked
bool ReadableStream::is_readable() const
{
    // A ReadableStream stream is readable if stream.[[state]] is "readable".
    return m_state == State::Readable;
}

// https://streams.spec.whatwg.org/#readablestream-closed
bool ReadableStream::is_closed() const
{
    // A ReadableStream stream is closed if stream.[[state]] is "closed".
    return m_state == State::Closed;
}

// https://streams.spec.whatwg.org/#readablestream-errored
bool ReadableStream::is_errored() const
{
    // A ReadableStream stream is errored if stream.[[state]] is "errored".
    return m_state == State::Errored;
}
// https://streams.spec.whatwg.org/#readablestream-locked
bool ReadableStream::is_locked() const
{
    // A ReadableStream stream is locked if ! IsReadableStreamLocked(stream) returns true.
    return is_readable_stream_locked(*this);
}

// https://streams.spec.whatwg.org/#is-readable-stream-disturbed
bool ReadableStream::is_disturbed() const
{
    // A ReadableStream stream is disturbed if stream.[[disturbed]] is true.
    return m_disturbed;
}

// https://streams.spec.whatwg.org/#readablestream-get-a-reader
WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> ReadableStream::get_a_reader()
{
    // To get a reader for a ReadableStream stream, return ? AcquireReadableStreamDefaultReader(stream). The result will be a ReadableStreamDefaultReader.
    return TRY(acquire_readable_stream_default_reader(*this));
}

// https://streams.spec.whatwg.org/#readablestream-pull-from-bytes
WebIDL::ExceptionOr<void> ReadableStream::pull_from_bytes(ByteBuffer bytes)
{
    auto& realm = this->realm();

    // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
    auto& controller = this->controller()->get<GC::Ref<ReadableByteStreamController>>();

    // 2. Let available be bytes’s length.
    auto available = bytes.size();

    // 3. Let desiredSize be available.
    auto desired_size = available;

    // 4. If stream’s current BYOB request view is non-null, then set desiredSize to stream’s current BYOB request
    //    view's byte length.
    if (auto byob_view = current_byob_request_view())
        desired_size = byob_view->byte_length();

    // 5. Let pullSize be the smaller value of available and desiredSize.
    auto pull_size = min(available, desired_size);

    // 6. Let pulled be the first pullSize bytes of bytes.
    auto pulled = pull_size == available ? move(bytes) : MUST(bytes.slice(0, pull_size));

    // 7. Remove the first pullSize bytes from bytes.
    if (pull_size != available)
        bytes = MUST(bytes.slice(pull_size, available - pull_size));

    // 8. If stream’s current BYOB request view is non-null, then:
    if (auto byob_view = current_byob_request_view()) {
        // 1. Write pulled into stream’s current BYOB request view.
        byob_view->write(pulled);

        // 2. Perform ? ReadableByteStreamControllerRespond(stream.[[controller]], pullSize).
        TRY(readable_byte_stream_controller_respond(controller, pull_size));
    }
    // 9. Otherwise,
    else {
        // 1. Set view to the result of creating a Uint8Array from pulled in stream’s relevant Realm.
        auto array_buffer = JS::ArrayBuffer::create(realm, move(pulled));
        auto view = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

        // 2. Perform ? ReadableByteStreamControllerEnqueue(stream.[[controller]], view).
        TRY(readable_byte_stream_controller_enqueue(controller, view));
    }

    return {};
}

// https://streams.spec.whatwg.org/#readablestream-current-byob-request-view
GC::Ptr<WebIDL::ArrayBufferView> ReadableStream::current_byob_request_view()
{
    // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
    VERIFY(m_controller->has<GC::Ref<ReadableByteStreamController>>());

    // 2. Let byobRequest be ! ReadableByteStreamControllerGetBYOBRequest(stream.[[controller]]).
    auto byob_request = readable_byte_stream_controller_get_byob_request(m_controller->get<GC::Ref<ReadableByteStreamController>>());

    // 3. If byobRequest is null, then return null.
    if (!byob_request)
        return {};

    // 4. Return byobRequest.[[view]].
    return byob_request->view();
}

// https://streams.spec.whatwg.org/#readablestream-enqueue
WebIDL::ExceptionOr<void> ReadableStream::enqueue(JS::Value chunk)
{
    VERIFY(m_controller.has_value());

    // 1. If stream.[[controller]] implements ReadableStreamDefaultController,
    if (m_controller->has<GC::Ref<ReadableStreamDefaultController>>()) {
        // 1. Perform ! ReadableStreamDefaultControllerEnqueue(stream.[[controller]], chunk).
        MUST(readable_stream_default_controller_enqueue(m_controller->get<GC::Ref<ReadableStreamDefaultController>>(), chunk));
    }
    // 2. Otherwise,
    else {
        // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
        VERIFY(m_controller->has<GC::Ref<ReadableByteStreamController>>());
        auto readable_byte_controller = m_controller->get<GC::Ref<ReadableByteStreamController>>();

        // 2. Assert: chunk is an ArrayBufferView.
        VERIFY(chunk.is_object());
        auto chunk_view = heap().allocate<WebIDL::ArrayBufferView>(chunk.as_object());

        // 3. Let byobView be the current BYOB request view for stream.
        auto byob_view = current_byob_request_view();

        // 4. If byobView is non-null, and chunk.[[ViewedArrayBuffer]] is byobView.[[ViewedArrayBuffer]], then:
        if (byob_view && chunk_view->viewed_array_buffer() == byob_view->viewed_array_buffer()) {
            // 1. Assert: chunk.[[ByteOffset]] is byobView.[[ByteOffset]].
            VERIFY(chunk_view->byte_offset() == byob_view->byte_offset());

            // 2. Assert: chunk.[[ByteLength]] ≤ byobView.[[ByteLength]].
            VERIFY(chunk_view->byte_length() <= byob_view->byte_length());

            // 3. Perform ? ReadableByteStreamControllerRespond(stream.[[controller]], chunk.[[ByteLength]]).
            TRY(readable_byte_stream_controller_respond(readable_byte_controller, chunk_view->byte_length()));
        }
        // 5. Otherwise, perform ? ReadableByteStreamControllerEnqueue(stream.[[controller]], chunk).
        else {
            TRY(readable_byte_stream_controller_enqueue(readable_byte_controller, chunk));
        }
    }

    return {};
}

// https://streams.spec.whatwg.org/#readablestream-set-up-with-byte-reading-support
void ReadableStream::set_up_with_byte_reading_support(GC::Ptr<PullAlgorithm> pull_algorithm, GC::Ptr<CancelAlgorithm> cancel_algorithm, double high_water_mark)
{
    auto& realm = this->realm();

    // 1. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); });

    // 2. Let pullAlgorithmWrapper be an algorithm that runs these steps:
    auto pull_algorithm_wrapper = GC::create_function(realm.heap(), [&realm, pull_algorithm]() {
        // 1. Let result be the result of running pullAlgorithm, if pullAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        GC::Ptr<JS::PromiseCapability> result = nullptr;
        if (pull_algorithm)
            result = pull_algorithm->function()();

        // 2. If result is a Promise, then return result.
        if (result != nullptr)
            return GC::Ref(*result);

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 3. Let cancelAlgorithmWrapper be an algorithm that runs these steps:
    auto cancel_algorithm_wrapper = GC::create_function(realm.heap(), [&realm, cancel_algorithm](JS::Value c) {
        // 1. Let result be the result of running cancelAlgorithm, if cancelAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        GC::Ptr<JS::PromiseCapability> result = nullptr;
        if (cancel_algorithm)
            result = cancel_algorithm->function()(c);

        // 2. If result is a Promise, then return result.
        if (result != nullptr)
            return GC::Ref(*result);

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Perform ! InitializeReadableStream(stream).
    // 5. Let controller be a new ReadableByteStreamController.
    auto controller = realm.create<ReadableByteStreamController>(realm);

    // 6. Perform ! SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithmWrapper, cancelAlgorithmWrapper, highWaterMark, undefined).
    MUST(set_up_readable_byte_stream_controller(*this, controller, start_algorithm, pull_algorithm_wrapper, cancel_algorithm_wrapper, high_water_mark, JS::js_undefined()));
}

// https://streams.spec.whatwg.org/#readablestream-pipe-through
GC::Ref<ReadableStream> ReadableStream::piped_through(GC::Ref<TransformStream> transform, bool prevent_close, bool prevent_abort, bool prevent_cancel, GC::Ptr<DOM::AbortSignal> signal)
{
    // 1. Assert: ! IsReadableStreamLocked(readable) is false.
    VERIFY(!is_readable_stream_locked(*this));

    // 2. Assert: ! IsWritableStreamLocked(transform.[[writable]]) is false.
    VERIFY(!is_writable_stream_locked(transform->writable()));

    // 3. Let signalArg be signal if signal was given, or undefined otherwise.
    // NOTE: Done by default arguments.

    // 4. Let promise be ! ReadableStreamPipeTo(readable, transform.[[writable]], preventClose, preventAbort, preventCancel, signalArg).
    auto promise = readable_stream_pipe_to(*this, transform->writable(), prevent_close, prevent_abort, prevent_cancel, signal);

    // 5. Set promise.[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*promise);

    // 6. Return transform.[[readable]].
    return transform->readable();
}

}
