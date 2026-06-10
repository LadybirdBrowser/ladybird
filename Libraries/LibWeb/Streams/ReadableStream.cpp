/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/ReadableStream.h>
#include <LibWeb/Bindings/UnderlyingSource.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamAsyncIterator.h>
#include <LibWeb/Streams/ReadableStreamBYOBReader.h>
#include <LibWeb/Streams/ReadableStreamBYOBRequest.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ReadableStream);

static UnderlyingSource underlying_source_from_bindings(Bindings::UnderlyingSource const& underlying_source)
{
    return {
        .auto_allocate_chunk_size = underlying_source.auto_allocate_chunk_size,
        .cancel = underlying_source.cancel,
        .pull = underlying_source.pull,
        .start = underlying_source.start,
        .is_bytes = underlying_source.type.has_value() && underlying_source.type.value() == Bindings::ReadableStreamType::Bytes,
    };
}

WebIDL::ExceptionOr<GC::Ref<ReadableStream>> ReadableStream::create_for_constructor(JS::Realm& realm, GC::Ptr<JS::Object> underlying_source_object, QueuingStrategy const& strategy)
{
    auto underlying_source = underlying_source_object ? JS::Value { underlying_source_object } : JS::js_null();
    auto underlying_source_dict = TRY(Bindings::convert_to_idl_value_for_underlying_source(realm.vm(), underlying_source));
    return create(realm, underlying_source_object, underlying_source_from_bindings(underlying_source_dict), strategy);
}

// https://streams.spec.whatwg.org/#rs-constructor
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> ReadableStream::create(JS::Realm& realm, GC::Ptr<JS::Object> underlying_source_object, UnderlyingSource const& underlying_source_dict, QueuingStrategy const& strategy)
{
    auto& vm = realm.vm();

    auto readable_stream = GC::Heap::the().allocate<ReadableStream>();

    // 1. If underlyingSource is missing, set it to null.
    auto underlying_source = underlying_source_object ? JS::Value(underlying_source_object) : JS::js_null();

    // 3. Perform ! InitializeReadableStream(this).

    // 4. If underlyingSourceDict["type"] is "bytes":
    if (underlying_source_dict.is_bytes) {
        // 1. If strategy["size"] exists, throw a RangeError exception.
        if (strategy.size)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Size strategy not allowed for byte stream"sv };

        // 2. Let highWaterMark be ? ExtractHighWaterMark(strategy, 0).
        auto high_water_mark = TRY(extract_high_water_mark(strategy, 0));

        // 3. Perform ? SetUpReadableByteStreamControllerFromUnderlyingSource(this, underlyingSource, underlyingSourceDict, highWaterMark).
        TRY(set_up_readable_byte_stream_controller_from_underlying_source(realm, *readable_stream, underlying_source, underlying_source_dict, high_water_mark));
    }
    // 5. Otherwise,
    else {
        // 1. Assert: underlyingSourceDict["type"] does not exist.
        VERIFY(!underlying_source_dict.is_bytes);

        // 2. Let sizeAlgorithm be ! ExtractSizeAlgorithm(strategy).
        auto size_algorithm = extract_size_algorithm(vm, strategy);

        // 3. Let highWaterMark be ? ExtractHighWaterMark(strategy, 1).
        auto high_water_mark = TRY(extract_high_water_mark(strategy, 1));

        // 4. Perform ? SetUpReadableStreamDefaultControllerFromUnderlyingSource(this, underlyingSource, underlyingSourceDict, highWaterMark, sizeAlgorithm).
        TRY(set_up_readable_stream_default_controller_from_underlying_source(realm, *readable_stream, underlying_source, underlying_source_dict, high_water_mark, size_algorithm));
    }

    return readable_stream;
}

// https://streams.spec.whatwg.org/#rs-from
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> ReadableStream::from(JS::Realm& realm, JS::Value async_iterable)
{
    // 1. Return ? ReadableStreamFromIterable(asyncIterable).
    return TRY(readable_stream_from_iterable(realm, async_iterable));
}

ReadableStream::ReadableStream()
{
}

}

namespace Web::Bindings {

Streams::ReadableStream* readable_stream_from_object(JS::Object& object)
{
    return Bindings::impl_from<Streams::ReadableStream>(&object);
}

void serialize_readable_stream_with_transfer(JS::Realm& realm, HTML::TransferDataEncoder& data_holder, GC::Ref<Streams::ReadableStream> stream)
{
    JS::Value wrapped_stream = Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, stream);
    auto result = MUST(HTML::structured_serialize_with_transfer(realm, wrapped_stream, { { wrapped_stream.as_object() } }));
    data_holder.extend(move(result.transfer_data_holders));
}

}

namespace Web::Streams {

ReadableStream::~ReadableStream() = default;

void ReadableStream::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_controller.has_value())
        m_controller->visit([&](auto& controller) { visitor.visit(controller); });
    visitor.visit(m_stored_error);
    if (m_reader.has_value())
        m_reader->visit([&](auto& reader) { visitor.visit(reader); });
}

// https://streams.spec.whatwg.org/#rs-locked
bool ReadableStream::locked() const
{
    // 1. Return ! IsReadableStreamLocked(this).
    return is_readable_stream_locked(*this);
}

// https://streams.spec.whatwg.org/#rs-cancel
GC::Ref<WebIDL::Promise> ReadableStream::cancel(JS::Realm& realm, Optional<JS::Value> reason)
{
    // 1. If ! IsReadableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
    if (is_readable_stream_locked(*this)) {
        auto exception = JS::TypeError::create(realm, "Cannot cancel a locked stream"sv);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 2. Return ! ReadableStreamCancel(this, reason).
    return readable_stream_cancel(realm, *this, reason.value_or(JS::js_undefined()));
}

// https://streams.spec.whatwg.org/#rs-get-reader
WebIDL::ExceptionOr<ReadableStreamReader> ReadableStream::get_reader(JS::Realm& realm, ReadableStreamGetReaderOptions const& options)
{
    // 1. If options["mode"] does not exist, return ? AcquireReadableStreamDefaultReader(this).
    if (!options.mode.has_value())
        return ReadableStreamReader { TRY(acquire_readable_stream_default_reader(realm, *this)) };

    // 3. Return ? AcquireReadableStreamBYOBReader(this).
    return ReadableStreamReader { TRY(acquire_readable_stream_byob_reader(realm, *this)) };
}

// https://streams.spec.whatwg.org/#rs-pipe-through
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> ReadableStream::pipe_through(JS::Realm& realm, ReadableWritablePair transform, StreamPipeOptions const& options)
{
    // 1. If ! IsReadableStreamLocked(this) is true, throw a TypeError exception.
    if (is_readable_stream_locked(*this))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Failed to execute 'pipeThrough' on 'ReadableStream': Cannot pipe a locked stream"sv };

    // 2. If ! IsWritableStreamLocked(transform["writable"]) is true, throw a TypeError exception.
    if (is_writable_stream_locked(*transform.writable))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Failed to execute 'pipeThrough' on 'ReadableStream': parameter 1's 'writable' is locked"sv };

    // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
    GC::Ptr<DOM::AbortSignal> signal = options.signal;

    // 4. Let promise be ! ReadableStreamPipeTo(this, transform["writable"], options["preventClose"], options["preventAbort"], options["preventCancel"], signal).
    auto promise = readable_stream_pipe_to(realm, *this, *transform.writable, options.prevent_close, options.prevent_abort, options.prevent_cancel, signal);

    // 5. Set promise.[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*promise);

    // 6. Return transform["readable"].
    return GC::Ref { *transform.readable };
}

// https://streams.spec.whatwg.org/#rs-pipe-to
GC::Ref<WebIDL::Promise> ReadableStream::pipe_to(JS::Realm& realm, WritableStream& destination, StreamPipeOptions const& options)
{
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
    GC::Ptr<DOM::AbortSignal> signal = options.signal;

    // 4. Return ! ReadableStreamPipeTo(this, destination, options["preventClose"], options["preventAbort"], options["preventCancel"], signal).
    return readable_stream_pipe_to(realm, *this, destination, options.prevent_close, options.prevent_abort, options.prevent_cancel, signal);
}

WebIDL::ExceptionOr<GC::Ref<ReadableStreamAsyncIterator>> ReadableStream::values(JS::Realm& realm, ReadableStreamIteratorOptions options)
{
    return ReadableStreamAsyncIterator::create(realm, JS::Object::PropertyKind::Value, *this, options);
}

// https://streams.spec.whatwg.org/#readablestream-tee
WebIDL::ExceptionOr<ReadableStreamPair> ReadableStream::tee(JS::Realm& realm)
{
    // To tee a ReadableStream stream, return ? ReadableStreamTee(stream, true).
    return TRY(readable_stream_tee(realm, *this, true));
}

// https://streams.spec.whatwg.org/#readablestream-close
void ReadableStream::close(JS::Realm& realm)
{
    controller()->visit(
        // 1. If stream.[[controller]] implements ReadableByteStreamController
        [&](GC::Ref<ReadableByteStreamController> controller) {
            // 1. Perform ! ReadableByteStreamControllerClose(stream.[[controller]]).
            MUST(readable_byte_stream_controller_close(realm, controller));

            // 2. If stream.[[controller]].[[pendingPullIntos]] is not empty, perform ! ReadableByteStreamControllerRespond(stream.[[controller]], 0).
            if (!controller->pending_pull_intos().is_empty())
                MUST(readable_byte_stream_controller_respond(realm, controller, 0));
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
WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> ReadableStream::get_a_reader(JS::Realm& realm)
{
    // To get a reader for a ReadableStream stream, return ? AcquireReadableStreamDefaultReader(stream). The result will be a ReadableStreamDefaultReader.
    return TRY(acquire_readable_stream_default_reader(realm, *this));
}

// https://streams.spec.whatwg.org/#readablestream-pull-from-bytes
WebIDL::ExceptionOr<void> ReadableStream::pull_from_bytes(JS::Realm& realm, ByteBuffer bytes)
{
    // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
    auto& controller = this->controller()->get<GC::Ref<ReadableByteStreamController>>();

    // 2. Let available be bytes’s length.
    auto available = bytes.size();

    // 3. Let desiredSize be available.
    auto desired_size = available;

    // 4. If stream’s current BYOB request view is non-null, then set desiredSize to stream’s current BYOB request
    //    view's byte length.
    if (auto byob_view = current_byob_request_view(realm); byob_view.has_value())
        desired_size = byob_view->byte_length();

    // 5. Let pullSize be the smaller value of available and desiredSize.
    auto pull_size = min(available, desired_size);

    // 6. Let pulled be the first pullSize bytes of bytes.
    auto pulled = pull_size == available ? move(bytes) : MUST(bytes.slice(0, pull_size));

    // 7. Remove the first pullSize bytes from bytes.
    // NB: We skip this step. No caller actually wants its bytes trimmed, and we don't take the bytes by reference anyways.

    // 8. If stream’s current BYOB request view is non-null, then:
    if (auto byob_view = current_byob_request_view(realm); byob_view.has_value()) {
        // 1. Write pulled into stream’s current BYOB request view.
        byob_view->write(pulled);

        // 2. Perform ? ReadableByteStreamControllerRespond(stream.[[controller]], pullSize).
        TRY(readable_byte_stream_controller_respond(realm, controller, pull_size));
    }
    // 9. Otherwise,
    else {
        // 1. Set view to the result of creating a Uint8Array from pulled in stream’s relevant Realm.
        auto array_buffer = JS::ArrayBuffer::create(realm, move(pulled));
        auto view = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

        // 2. Perform ? ReadableByteStreamControllerEnqueue(stream.[[controller]], view).
        TRY(readable_byte_stream_controller_enqueue(realm, controller, view));
    }

    return {};
}

// https://streams.spec.whatwg.org/#readablestream-current-byob-request-view
Optional<WebIDL::ArrayBufferView> ReadableStream::current_byob_request_view(JS::Realm& realm)
{
    // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
    VERIFY(m_controller->has<GC::Ref<ReadableByteStreamController>>());

    // 2. Let byobRequest be ! ReadableByteStreamControllerGetBYOBRequest(stream.[[controller]]).
    auto byob_request = readable_byte_stream_controller_get_byob_request(realm, m_controller->get<GC::Ref<ReadableByteStreamController>>());

    // 3. If byobRequest is null, then return null.
    if (!byob_request)
        return {};

    // 4. Return byobRequest.[[view]].
    auto view = byob_request->view();
    VERIFY(!view.has<Empty>());
    return WebIDL::ArrayBufferView { view.downcast<WebIDL::ArrayBufferViewVariant>() };
}

// https://streams.spec.whatwg.org/#readablestream-enqueue
WebIDL::ExceptionOr<void> ReadableStream::enqueue(JS::Realm& realm, JS::Value chunk)
{
    VERIFY(m_controller.has_value());

    // 1. If stream.[[controller]] implements ReadableStreamDefaultController,
    if (m_controller->has<GC::Ref<ReadableStreamDefaultController>>()) {
        // 1. Perform ! ReadableStreamDefaultControllerEnqueue(stream.[[controller]], chunk).
        MUST(readable_stream_default_controller_enqueue(realm, m_controller->get<GC::Ref<ReadableStreamDefaultController>>(), chunk));
    }
    // 2. Otherwise,
    else {
        // 1. Assert: stream.[[controller]] implements ReadableByteStreamController.
        VERIFY(m_controller->has<GC::Ref<ReadableByteStreamController>>());
        auto readable_byte_controller = m_controller->get<GC::Ref<ReadableByteStreamController>>();

        // 2. Assert: chunk is an ArrayBufferView.
        VERIFY(chunk.is_object());
        WebIDL::ArrayBufferView chunk_view { WebIDL::ArrayBufferView::from_object(chunk.as_object()) };

        // 3. Let byobView be the current BYOB request view for stream.
        auto byob_view = current_byob_request_view(realm);

        // 4. If byobView is non-null, and chunk.[[ViewedArrayBuffer]] is byobView.[[ViewedArrayBuffer]], then:
        if (byob_view.has_value() && chunk_view.viewed_array_buffer() == byob_view->viewed_array_buffer()) {
            // 1. Assert: chunk.[[ByteOffset]] is byobView.[[ByteOffset]].
            VERIFY(chunk_view.byte_offset() == byob_view->byte_offset());

            // 2. Assert: chunk.[[ByteLength]] ≤ byobView.[[ByteLength]].
            VERIFY(chunk_view.byte_length() <= byob_view->byte_length());

            // 3. Perform ? ReadableByteStreamControllerRespond(stream.[[controller]], chunk.[[ByteLength]]).
            TRY(readable_byte_stream_controller_respond(realm, readable_byte_controller, chunk_view.byte_length()));
        }
        // 5. Otherwise, perform ? ReadableByteStreamControllerEnqueue(stream.[[controller]], chunk).
        else {
            TRY(readable_byte_stream_controller_enqueue(realm, readable_byte_controller, chunk));
        }
    }

    return {};
}

// https://streams.spec.whatwg.org/#readablestream-set-up-with-byte-reading-support
void ReadableStream::set_up_with_byte_reading_support(JS::Realm& realm, GC::Ptr<PullAlgorithm> pull_algorithm, GC::Ptr<CancelAlgorithm> cancel_algorithm, double high_water_mark)
{
    // 1. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(GC::Heap::the(), []() -> WebIDL::ExceptionOr<JS::Value> { return JS::js_undefined(); });

    // 2. Let pullAlgorithmWrapper be an algorithm that runs these steps:
    auto pull_algorithm_wrapper = GC::create_function(GC::Heap::the(), [&realm, pull_algorithm]() {
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
    auto cancel_algorithm_wrapper = GC::create_function(GC::Heap::the(), [&realm, cancel_algorithm](JS::Value c) {
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
    auto controller = GC::Heap::the().allocate<ReadableByteStreamController>();

    // 6. Perform ! SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithmWrapper, cancelAlgorithmWrapper, highWaterMark, undefined).
    MUST(set_up_readable_byte_stream_controller(realm, *this, controller, start_algorithm, pull_algorithm_wrapper, cancel_algorithm_wrapper, high_water_mark, JS::js_undefined()));
}

// https://streams.spec.whatwg.org/#readablestream-pipe-through
GC::Ref<ReadableStream> ReadableStream::piped_through(JS::Realm& realm, GC::Ref<TransformStream> transform, bool prevent_close, bool prevent_abort, bool prevent_cancel, GC::Ptr<DOM::AbortSignal> signal)
{
    // 1. Assert: ! IsReadableStreamLocked(readable) is false.
    VERIFY(!is_readable_stream_locked(*this));

    // 2. Assert: ! IsWritableStreamLocked(transform.[[writable]]) is false.
    VERIFY(!is_writable_stream_locked(transform->writable()));

    // 3. Let signalArg be signal if signal was given, or undefined otherwise.
    // NOTE: Done by default arguments.

    // 4. Let promise be ! ReadableStreamPipeTo(readable, transform.[[writable]], preventClose, preventAbort, preventCancel, signalArg).
    auto promise = readable_stream_pipe_to(realm, *this, transform->writable(), prevent_close, prevent_abort, prevent_cancel, signal);

    // 5. Set promise.[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*promise);

    // 6. Return transform.[[readable]].
    return transform->readable();
}

// https://streams.spec.whatwg.org/#ref-for-transfer-steps
WebIDL::ExceptionOr<void> ReadableStream::transfer_steps(JS::Realm& realm, HTML::TransferDataEncoder& data_holder)
{
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. If ! IsReadableStreamLocked(value) is true, throw a "DataCloneError" DOMException.
    if (is_readable_stream_locked(*this))
        return WebIDL::DataCloneError::create("Cannot transfer locked ReadableStream"_utf16);

    // 2. Let port1 be a new MessagePort in the current Realm.
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);
    auto port1 = HTML::MessagePort::create(global_scope->this_impl());

    // 3. Let port2 be a new MessagePort in the current Realm.
    auto port2 = HTML::MessagePort::create(global_scope->this_impl());

    // 4. Entangle port1 and port2.
    port1->entangle_with(port2);

    // 5. Let writable be a new WritableStream in the current Realm.
    auto writable = GC::Heap::the().allocate<WritableStream>();

    // 6. Perform ! SetUpCrossRealmTransformWritable(writable, port1).
    set_up_cross_realm_transform_writable(realm, writable, port1);

    // 7. Let promise be ! ReadableStreamPipeTo(value, writable, false, false, false).
    auto promise = readable_stream_pipe_to(realm, *this, writable, false, false, false);

    // 8. Set promise.[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(promise);

    // 9. Set dataHolder.[[port]] to ! StructuredSerializeWithTransfer(port2, « port2 »).
    Bindings::serialize_message_port_with_transfer(realm, data_holder, port2);

    return {};
}

// https://streams.spec.whatwg.org/#ref-for-transfer-receiving-steps
WebIDL::ExceptionOr<void> ReadableStream::transfer_receiving_steps(JS::Realm& realm, HTML::TransferDataDecoder& data_holder)
{
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. Let deserializedRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[port]], the current Realm).
    auto deserialized_record = MUST(HTML::structured_deserialize_with_transfer_internal(data_holder, realm));

    // 2. Let port be deserializedRecord.[[Deserialized]].
    auto* port = Bindings::message_port_from_object(deserialized_record.as_object());
    VERIFY(port);

    // 3. Perform ! SetUpCrossRealmTransformReadable(value, port).
    set_up_cross_realm_transform_readable(realm, *this, *port);

    return {};
}

}
