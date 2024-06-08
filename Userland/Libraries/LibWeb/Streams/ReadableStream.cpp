/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ReadableStreamPrototype.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamBYOBReader.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/UnderlyingSource.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

JS_DEFINE_ALLOCATOR(ReadableStream);

// https://streams.spec.whatwg.org/#rs-constructor
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> ReadableStream::construct_impl(JS::Realm& realm, Optional<JS::Handle<JS::Object>> const& underlying_source_object, QueuingStrategy const& strategy)
{
    auto& vm = realm.vm();

    auto readable_stream = realm.heap().allocate<ReadableStream>(realm, realm);

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
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> ReadableStream::from(JS::VM& vm, JS::Value async_iterable)
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
JS::NonnullGCPtr<JS::Object> ReadableStream::cancel(JS::Value reason)
{
    auto& realm = this->realm();

    // 1. If ! IsReadableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
    if (is_readable_stream_locked(*this)) {
        auto exception = JS::TypeError::create(realm, "Cannot cancel a locked stream"sv);
        return WebIDL::create_rejected_promise(realm, JS::Value { exception })->promise();
    }

    // 2. Return ! ReadableStreamCancel(this, reason).
    return readable_stream_cancel(*this, reason)->promise();
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

WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> ReadableStream::pipe_through(ReadableWritablePair transform, StreamPipeOptions const& options)
{
    // 1. If ! IsReadableStreamLocked(this) is true, throw a TypeError exception.
    if (is_readable_stream_locked(*this))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Failed to execute 'pipeThrough' on 'ReadableStream': Cannot pipe a locked stream"sv };

    // 2. If ! IsWritableStreamLocked(transform["writable"]) is true, throw a TypeError exception.
    if (is_writable_stream_locked(*transform.writable))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Failed to execute 'pipeThrough' on 'ReadableStream': parameter 1's 'writable' is locked"sv };

    // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
    auto signal = options.signal ? JS::Value(options.signal) : JS::js_undefined();

    // 4. Let promise be ! ReadableStreamPipeTo(this, transform["writable"], options["preventClose"], options["preventAbort"], options["preventCancel"], signal).
    auto promise = readable_stream_pipe_to(*this, *transform.writable, options.prevent_close, options.prevent_abort, options.prevent_cancel, signal);

    // 5. Set promise.[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*promise);

    // 6. Return transform["readable"].
    return JS::NonnullGCPtr { *transform.readable };
}

JS::NonnullGCPtr<JS::Object> ReadableStream::pipe_to(WritableStream& destination, StreamPipeOptions const& options)
{
    auto& realm = this->realm();

    // 1. If ! IsReadableStreamLocked(this) is true, return a promise rejected with a TypeError exception.
    if (is_readable_stream_locked(*this)) {
        auto promise = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to execute 'pipeTo' on 'ReadableStream': Cannot pipe a locked stream"sv));
        return promise->promise();
    }

    // 2. If ! IsWritableStreamLocked(destination) is true, return a promise rejected with a TypeError exception.
    if (is_writable_stream_locked(destination)) {
        auto promise = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to execute 'pipeTo' on 'ReadableStream':  Cannot pipe to a locked stream"sv));
        return promise->promise();
    }

    // 3. Let signal be options["signal"] if it exists, or undefined otherwise.
    auto signal = options.signal ? JS::Value(options.signal) : JS::js_undefined();

    // 4. Return ! ReadableStreamPipeTo(this, destination, options["preventClose"], options["preventAbort"], options["preventCancel"], signal).
    return readable_stream_pipe_to(*this, destination, options.prevent_close, options.prevent_abort, options.prevent_cancel, signal)->promise();
}

// https://streams.spec.whatwg.org/#readablestream-tee
WebIDL::ExceptionOr<ReadableStreamPair> ReadableStream::tee()
{
    // To tee a ReadableStream stream, return ? ReadableStreamTee(stream, true).
    return TRY(readable_stream_tee(realm(), *this, true));
}

// https://streams.spec.whatwg.org/#readablestream-close
void ReadableStream::close()
{
    controller()->visit(
        // 1. If stream.[[controller]] implements ReadableByteStreamController
        [&](JS::NonnullGCPtr<ReadableByteStreamController> controller) {
            // 1. Perform ! ReadableByteStreamControllerClose(stream.[[controller]]).
            MUST(readable_byte_stream_controller_close(controller));

            // 2. If stream.[[controller]].[[pendingPullIntos]] is not empty, perform ! ReadableByteStreamControllerRespond(stream.[[controller]], 0).
            if (!controller->pending_pull_intos().is_empty())
                MUST(readable_byte_stream_controller_respond(controller, 0));
        },

        // 2. Otherwise, perform ! ReadableStreamDefaultControllerClose(stream.[[controller]]).
        [&](JS::NonnullGCPtr<ReadableStreamDefaultController> controller) {
            readable_stream_default_controller_close(*controller);
        });
}

// https://streams.spec.whatwg.org/#readablestream-error
void ReadableStream::error(JS::Value error)
{
    controller()->visit(
        // 1. If stream.[[controller]] implements ReadableByteStreamController, then perform
        //    ! ReadableByteStreamControllerError(stream.[[controller]], e).
        [&](JS::NonnullGCPtr<ReadableByteStreamController> controller) {
            readable_byte_stream_controller_error(controller, error);
        },

        // 2. Otherwise, perform ! ReadableStreamDefaultControllerError(stream.[[controller]], e).
        [&](JS::NonnullGCPtr<ReadableStreamDefaultController> controller) {
            readable_stream_default_controller_error(controller, error);
        });
}

void ReadableStream::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ReadableStream);
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

}
