/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataViewConstructor.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStreamBYOBReader.h>
#include <LibWeb/Streams/ReadableStreamBYOBRequest.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/ReadableStreamGenericReader.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/ReadableStreamPipeTo.h>
#include <LibWeb/Streams/ReadableStreamTee.h>
#include <LibWeb/Streams/UnderlyingSource.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultWriter.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#acquire-readable-stream-byob-reader
WebIDL::ExceptionOr<GC::Ref<ReadableStreamBYOBReader>> acquire_readable_stream_byob_reader(ReadableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let reader be a new ReadableStreamBYOBReader.
    auto reader = realm.create<ReadableStreamBYOBReader>(realm);

    // 2. Perform ? SetUpReadableStreamBYOBReader(reader, stream).
    TRY(set_up_readable_stream_byob_reader(reader, stream));

    // 3. Return reader.
    return reader;
}

// https://streams.spec.whatwg.org/#acquire-readable-stream-reader
WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> acquire_readable_stream_default_reader(ReadableStream& stream)
{
    auto& realm = stream.realm();

    // 1. Let reader be a new ReadableStreamDefaultReader.
    auto reader = realm.create<ReadableStreamDefaultReader>(realm);

    // 2. Perform ? SetUpReadableStreamDefaultReader(reader, stream).
    TRY(set_up_readable_stream_default_reader(reader, stream));

    // 3. Return reader.
    return reader;
}

// AD-HOC: Can be used instead of CreateReadableStream in cases where we need to set up a newly allocated ReadableStream
//         before initialization of said ReadableStream, i.e. ReadableStream is captured by lambdas in an uninitialized
//         state.
//
// https://streams.spec.whatwg.org/#create-readable-stream
static WebIDL::ExceptionOr<void> create_readable_stream_with_existing_stream(JS::Realm& realm, ReadableStream& stream, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<PullAlgorithm> pull_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm, Optional<double> high_water_mark = {}, GC::Ptr<SizeAlgorithm> size_algorithm = {})
{
    // 1. If highWaterMark was not passed, set it to 1.
    if (!high_water_mark.has_value())
        high_water_mark = 1.0;

    // 2. If sizeAlgorithm was not passed, set it to an algorithm that returns 1.
    if (!size_algorithm)
        size_algorithm = GC::create_function(realm.heap(), [](JS::Value) { return JS::normal_completion(JS::Value(1)); });

    // 3. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
    VERIFY(is_non_negative_number(JS::Value { *high_water_mark }));

    // 4. Let stream be a new ReadableStream.
    //    NOTE: The ReadableStream is allocated outside the scope of this method.

    // 5. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(stream);

    // 6. Let controller be a new ReadableStreamDefaultController.
    auto controller = realm.create<ReadableStreamDefaultController>(realm);

    // 7. Perform ? SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm).
    TRY(set_up_readable_stream_default_controller(stream, controller, start_algorithm, pull_algorithm, cancel_algorithm, *high_water_mark, *size_algorithm));

    return {};
}

// https://streams.spec.whatwg.org/#create-readable-stream
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> create_readable_stream(JS::Realm& realm, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<PullAlgorithm> pull_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm, Optional<double> high_water_mark, GC::Ptr<SizeAlgorithm> size_algorithm)
{
    auto stream = realm.create<ReadableStream>(realm);
    TRY(create_readable_stream_with_existing_stream(realm, stream, start_algorithm, pull_algorithm, cancel_algorithm, high_water_mark, size_algorithm));

    return stream;
}

// https://streams.spec.whatwg.org/#abstract-opdef-createreadablebytestream
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> create_readable_byte_stream(JS::Realm& realm, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<PullAlgorithm> pull_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm)
{
    // 1. Let stream be a new ReadableStream.
    auto stream = realm.create<ReadableStream>(realm);

    // 2. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(stream);

    // 3. Let controller be a new ReadableByteStreamController.
    auto controller = realm.create<ReadableByteStreamController>(realm);

    // 4. Perform ? SetUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, 0, undefined).
    TRY(set_up_readable_byte_stream_controller(stream, controller, start_algorithm, pull_algorithm, cancel_algorithm, 0, JS::js_undefined()));

    // 5. Return stream.
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

// https://streams.spec.whatwg.org/#is-readable-stream-locked
bool is_readable_stream_locked(ReadableStream const& stream)
{
    // 1. If stream.[[reader]] is undefined, return false.
    if (!stream.reader().has_value())
        return false;

    // 2. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#readable-stream-from-iterable
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> readable_stream_from_iterable(JS::VM& vm, JS::Value async_iterable)
{
    auto& realm = *vm.current_realm();

    // 1. Let stream be undefined.
    // AD-HOC: We capture 'stream' in a lambda later, so it needs to be allocated now.
    auto stream = realm.create<ReadableStream>(realm);

    // 2. Let iteratorRecord be ? GetIterator(asyncIterable, async).
    auto iterator_record = TRY(JS::get_iterator(vm, async_iterable, JS::IteratorHint::Async));

    // 3. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 4. Let pullAlgorithm be the following steps:
    auto pull_algorithm = GC::create_function(realm.heap(), [&vm, &realm, stream, iterator_record]() mutable {
        // 1.  Let nextResult be IteratorNext(iteratorRecord).
        auto next_result = JS::iterator_next(vm, iterator_record);

        // 2. If nextResult is an abrupt completion, return a promise rejected with nextResult.[[Value]].
        if (next_result.is_error())
            return WebIDL::create_rejected_promise(realm, next_result.throw_completion().release_value());

        // 3. Let nextPromise be a promise resolved with nextResult.[[Value]].
        auto next_promise = WebIDL::create_resolved_promise(realm, next_result.release_value());

        // 4. Return the result of reacting to nextPromise with the following fulfillment steps, given iterResult:
        return WebIDL::upon_fulfillment(next_promise,
            GC::create_function(realm.heap(), [&vm, stream](JS::Value iter_result) -> WebIDL::ExceptionOr<JS::Value> {
                // 1. If iterResult is not an Object, throw a TypeError.
                if (!iter_result.is_object())
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "iterResult is not an Object"sv };

                // 2. Let done be ? IteratorComplete(iterResult).
                auto done = TRY(JS::iterator_complete(vm, iter_result.as_object()));

                // 3. If done is true:
                if (done) {
                    // 1. Perform ! ReadableStreamDefaultControllerClose(stream.[[controller]]).
                    readable_stream_default_controller_close(stream->controller()->get<GC::Ref<ReadableStreamDefaultController>>());
                }
                // 4. Otherwise:
                else {
                    // 1. Let value be ? IteratorValue(iterResult).
                    auto value = TRY(JS::iterator_value(vm, iter_result.as_object()));

                    // 2. Perform ! ReadableStreamDefaultControllerEnqueue(stream.[[controller]], value).
                    MUST(readable_stream_default_controller_enqueue(stream->controller()->get<GC::Ref<ReadableStreamDefaultController>>(), value));
                }

                return JS::js_undefined();
            }));
    });

    // 5. Let cancelAlgorithm be the following steps, given reason:
    auto cancel_algorithm = GC::create_function(realm.heap(), [&vm, &realm, iterator_record](JS::Value reason) {
        // 1. Let iterator be iteratorRecord.[[Iterator]].
        auto iterator = iterator_record->iterator;

        // 2. Let returnMethod be GetMethod(iterator, "return").
        auto return_method = iterator->get(vm.names.return_);

        // 3. If returnMethod is an abrupt completion, return a promise rejected with returnMethod.[[Value]].
        if (return_method.is_error())
            return WebIDL::create_rejected_promise(realm, return_method.throw_completion().release_value());

        // 4. If returnMethod.[[Value]] is undefined, return a promise resolved with undefined.
        if (return_method.value().is_undefined())
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());

        // 5. Let returnResult be Call(returnMethod.[[Value]], iterator, « reason »).
        auto return_result = JS::call(vm, return_method.value(), reason);

        // 6. If returnResult is an abrupt completion, return a promise rejected with returnResult.[[Value]].
        if (return_result.is_error())
            return WebIDL::create_rejected_promise(realm, return_result.throw_completion().release_value());

        // 7. Let returnPromise be a promise resolved with returnResult.[[Value]].
        auto return_promise = WebIDL::create_resolved_promise(realm, return_result.release_value());

        // 8. Return the result of reacting to returnPromise with the following fulfillment steps, given iterResult:
        return WebIDL::upon_fulfillment(return_promise,
            GC::create_function(realm.heap(), [](JS::Value iter_result) -> WebIDL::ExceptionOr<JS::Value> {
                // 1. If iterResult is not an Object, throw a TypeError.
                if (!iter_result.is_object())
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "iterResult is not an Object"sv };

                // 2. Return undefined.
                return JS::js_undefined();
            }));
    });

    // 6. Set stream to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, 0).
    MUST(create_readable_stream_with_existing_stream(realm, stream, start_algorithm, pull_algorithm, cancel_algorithm, 0));

    // 7. Return stream.
    return stream;
}

// https://streams.spec.whatwg.org/#readable-stream-pipe-to
GC::Ref<WebIDL::Promise> readable_stream_pipe_to(ReadableStream& source, WritableStream& dest, bool prevent_close, bool prevent_abort, bool prevent_cancel, GC::Ptr<DOM::AbortSignal> signal)
{
    auto& realm = source.realm();

    // 1. Assert: source implements ReadableStream.
    // 2. Assert: dest implements WritableStream.
    // 3. Assert: preventClose, preventAbort, and preventCancel are all booleans.

    // 4. If signal was not given, let signal be undefined.
    // 5. Assert: either signal is undefined, or signal implements AbortSignal.

    // 6. Assert: ! IsReadableStreamLocked(source) is false.
    VERIFY(!is_readable_stream_locked(source));

    // 7. Assert: ! IsWritableStreamLocked(dest) is false.
    VERIFY(!is_writable_stream_locked(dest));

    // 8. If source.[[controller]] implements ReadableByteStreamController, let reader be either ! AcquireReadableStreamBYOBReader(source)
    //    or ! AcquireReadableStreamDefaultReader(source), at the user agent’s discretion.
    // 9. Otherwise, let reader be ! AcquireReadableStreamDefaultReader(source).
    auto reader = MUST(source.controller()->visit([](auto const& controller) {
        return acquire_readable_stream_default_reader(*controller->stream());
    }));

    // 10. Let writer be ! AcquireWritableStreamDefaultWriter(dest).
    auto writer = MUST(acquire_writable_stream_default_writer(dest));

    // 11. Set source.[[disturbed]] to true.
    source.set_disturbed(true);

    // 12. Let shuttingDown be false.
    // NOTE: This is internal to the ReadableStreamPipeTo class.

    // 13. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    auto operation = realm.heap().allocate<Detail::ReadableStreamPipeTo>(realm, promise, source, dest, reader, writer, prevent_close, prevent_abort, prevent_cancel);

    // 14. If signal is not undefined,
    if (signal) {
        // 1. Let abortAlgorithm be the following steps:
        auto abort_algorithm = [&realm, operation, source = GC::Ref { source }, dest = GC::Ref { dest }, prevent_abort, prevent_cancel, signal]() {
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. Let error be signal’s abort reason.
            auto error = signal->reason();

            // 2. Let actions be an empty ordered set.
            GC::Ptr<GC::Function<GC::Ref<WebIDL::Promise>()>> abort_destination;
            GC::Ptr<GC::Function<GC::Ref<WebIDL::Promise>()>> cancel_source;

            // 3. If preventAbort is false, append the following action to actions:
            if (!prevent_abort) {
                abort_destination = GC::create_function(realm.heap(), [&realm, dest, error]() {
                    // 1. If dest.[[state]] is "writable", return ! WritableStreamAbort(dest, error).
                    if (dest->state() == WritableStream::State::Writable)
                        return writable_stream_abort(dest, error);

                    // 2. Otherwise, return a promise resolved with undefined.
                    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
                });
            }

            // 4. If preventCancel is false, append the following action action to actions:
            if (!prevent_cancel) {
                cancel_source = GC::create_function(realm.heap(), [&realm, source, error]() {
                    // 1. If source.[[state]] is "readable", return ! ReadableStreamCancel(source, error).
                    if (source->state() == ReadableStream::State::Readable)
                        return readable_stream_cancel(source, error);

                    // 2. Otherwise, return a promise resolved with undefined.
                    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
                });
            }

            // 5. Shutdown with an action consisting of getting a promise to wait for all of the actions in actions, and with error.
            auto action = GC::create_function(realm.heap(), [&realm, abort_destination, cancel_source]() {
                GC::RootVector<GC::Ref<WebIDL::Promise>> actions(realm.heap());

                if (abort_destination)
                    actions.append(abort_destination->function()());
                if (cancel_source)
                    actions.append(cancel_source->function()());

                return WebIDL::get_promise_for_wait_for_all(realm, actions);
            });

            operation->shutdown_with_action(action, error);
        };

        // 2. If signal is aborted, perform abortAlgorithm and return promise.
        if (signal->aborted()) {
            abort_algorithm();
            return promise;
        }

        // 3. Add abortAlgorithm to signal.
        auto signal_id = signal->add_abort_algorithm(move(abort_algorithm));
        operation->set_abort_signal(*signal, signal_id.value());
    }

    // 15. In parallel (but not really; see #905), using reader and writer, read all chunks from source and write them
    //     to dest. Due to the locking provided by the reader and writer, the exact manner in which this happens is not
    //     observable to author code, and so there is flexibility in how this is done.
    operation->process();

    // 16. Return promise.
    return promise;
}

// https://streams.spec.whatwg.org/#readable-stream-tee
WebIDL::ExceptionOr<ReadableStreamPair> readable_stream_tee(JS::Realm& realm, ReadableStream& stream, bool clone_for_branch2)
{
    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: cloneForBranch2 is a boolean.

    // 3. If stream.[[controller]] implements ReadableByteStreamController, return ? ReadableByteStreamTee(stream).
    if (stream.controller()->has<GC::Ref<Streams::ReadableByteStreamController>>())
        return TRY(readable_byte_stream_tee(realm, stream));

    // 4. Return ? ReadableStreamDefaultTee(stream, cloneForBranch2).
    return TRY(readable_stream_default_tee(realm, stream, clone_for_branch2));
}

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
    auto params = realm.create<Detail::ReadableStreamTeeParams>();

    // 12. Let cancelPromise be a new promise.
    auto cancel_promise = WebIDL::create_promise(realm);

    // 13. Let pullAlgorithm be the following steps:
    auto pull_algorithm = GC::create_function(realm.heap(), [&realm, &stream, reader, params, cancel_promise, clone_for_branch2]() {
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
        auto read_request = realm.heap().allocate<Detail::ReadableStreamTeeReadRequest>(realm, stream, params, cancel_promise, clone_for_branch2);

        // 4. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        readable_stream_default_reader_read(reader, read_request);

        // 5. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // AD-HOC: The read request within the pull algorithm must be able to re-invoke the pull algorithm, so cache it here.
    params->pull_algorithm = pull_algorithm;

    // 14. Let cancel1Algorithm be the following steps, taking a reason argument:
    auto cancel1_algorithm = GC::create_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
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
            WebIDL::resolve_promise(realm, cancel_promise, cancel_result->promise());
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 15. Let cancel2Algorithm be the following steps, taking a reason argument:
    auto cancel2_algorithm = GC::create_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
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
            WebIDL::resolve_promise(realm, cancel_promise, cancel_result->promise());
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 16. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 17. Set branch1 to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancel1Algorithm).
    params->branch1 = MUST(create_readable_stream(realm, start_algorithm, pull_algorithm, cancel1_algorithm));

    // 18. Set branch2 to ! CreateReadableStream(startAlgorithm, pullAlgorithm, cancel2Algorithm).
    params->branch2 = MUST(create_readable_stream(realm, start_algorithm, pull_algorithm, cancel2_algorithm));

    // 19. Upon rejection of reader.[[closedPromise]] with reason r,
    WebIDL::upon_rejection(*reader->closed_promise_capability(), GC::create_function(realm.heap(), [&realm, params, cancel_promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        auto controller1 = params->branch1->controller()->get<GC::Ref<ReadableStreamDefaultController>>();
        auto controller2 = params->branch2->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

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

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
WebIDL::ExceptionOr<ReadableStreamPair> readable_byte_stream_tee(JS::Realm& realm, ReadableStream& stream)
{
    // 1. Assert: stream implements ReadableStream.
    // 2. Assert: stream.[[controller]] implements ReadableByteStreamController.
    VERIFY(stream.controller().has_value() && stream.controller()->has<GC::Ref<ReadableByteStreamController>>());

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
    auto params = realm.create<Detail::ReadableByteStreamTeeParams>(reader);

    // 13. Let cancelPromise be a new promise.
    auto cancel_promise = WebIDL::create_promise(realm);

    // 14. Let forwardReaderError be the following steps, taking a thisReader argument:
    auto forward_reader_error = GC::create_function(realm.heap(), [&realm, params, cancel_promise](ReadableStreamReader const& this_reader) {
        // 1. Upon rejection of thisReader.[[closedPromise]] with reason r,
        auto closed_promise = this_reader.visit([](auto underlying_reader) { return underlying_reader->closed_promise_capability(); });

        WebIDL::upon_rejection(*closed_promise, GC::create_function(realm.heap(), [&realm, this_reader, params, cancel_promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            auto controller1 = params->branch1->controller()->get<GC::Ref<ReadableByteStreamController>>();
            auto controller2 = params->branch2->controller()->get<GC::Ref<ReadableByteStreamController>>();

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
    auto pull_with_default_reader = GC::create_function(realm.heap(), [&realm, &stream, params, cancel_promise, forward_reader_error]() mutable {
        // 1. If reader implements ReadableStreamBYOBReader,
        if (auto const* byob_reader = params->reader.get_pointer<GC::Ref<ReadableStreamBYOBReader>>()) {
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
        auto read_request = realm.heap().allocate<Detail::ReadableByteStreamTeeDefaultReadRequest>(realm, stream, params, cancel_promise);

        // 3. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
        readable_stream_default_reader_read(params->reader.get<GC::Ref<ReadableStreamDefaultReader>>(), read_request);
    });

    // 16. Let pullWithBYOBReader be the following steps, given view and forBranch2:
    auto pull_with_byob_reader = GC::create_function(realm.heap(), [&realm, &stream, params, cancel_promise, forward_reader_error](GC::Ref<WebIDL::ArrayBufferView> view, bool for_branch2) mutable {
        // 1. If reader implements ReadableStreamDefaultReader,
        if (auto const* default_reader = params->reader.get_pointer<GC::Ref<ReadableStreamDefaultReader>>()) {
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
        auto read_into_request = realm.heap().allocate<Detail::ReadableByteStreamTeeBYOBReadRequest>(realm, stream, params, cancel_promise, *byob_branch, *other_branch, for_branch2);

        // 5. Perform ! ReadableStreamBYOBReaderRead(reader, view, 1, readIntoRequest).
        readable_stream_byob_reader_read(params->reader.get<GC::Ref<ReadableStreamBYOBReader>>(), view, 1, read_into_request);
    });

    // 17. Let pull1Algorithm be the following steps:
    auto pull1_algorithm = GC::create_function(realm.heap(), [&realm, params, pull_with_default_reader, pull_with_byob_reader]() {
        auto controller1 = params->branch1->controller()->get<GC::Ref<ReadableByteStreamController>>();

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
    auto pull2_algorithm = GC::create_function(realm.heap(), [&realm, params, pull_with_default_reader, pull_with_byob_reader]() {
        auto controller2 = params->branch2->controller()->get<GC::Ref<ReadableByteStreamController>>();

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
    auto cancel1_algorithm = GC::create_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
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
            WebIDL::resolve_promise(realm, cancel_promise, cancel_result->promise());
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 20. Let cancel2Algorithm be the following steps, taking a reason argument:
    auto cancel2_algorithm = GC::create_function(realm.heap(), [&realm, &stream, params, cancel_promise](JS::Value reason) {
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
            WebIDL::resolve_promise(realm, cancel_promise, cancel_result->promise());
        }

        // 4. Return cancelPromise.
        return cancel_promise;
    });

    // 21. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
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

// https://streams.spec.whatwg.org/#readable-stream-add-read-into-request
void readable_stream_add_read_into_request(ReadableStream& stream, GC::Ref<ReadIntoRequest> read_into_request)
{
    VERIFY(stream.reader().has_value());

    // 1. Assert: stream.[[reader]] implements ReadableStreamBYOBReader.
    auto reader = stream.reader()->get<GC::Ref<ReadableStreamBYOBReader>>();

    // 2. Assert: stream.[[state]] is "readable" or "closed".
    VERIFY(first_is_one_of(stream.state(), ReadableStream::State::Readable, ReadableStream::State::Closed));

    // 3. Append readRequest to stream.[[reader]].[[readIntoRequests]].
    reader->read_into_requests().append(read_into_request);
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-request
void readable_stream_add_read_request(ReadableStream& stream, GC::Ref<ReadRequest> read_request)
{
    VERIFY(stream.reader().has_value());

    // 1. Assert: stream.[[reader]] implements ReadableStreamDefaultReader.
    auto reader = stream.reader()->get<GC::Ref<ReadableStreamDefaultReader>>();

    // 2. Assert: stream.[[state]] is "readable".
    VERIFY(stream.state() == ReadableStream::State::Readable);

    // 3. Append readRequest to stream.[[reader]].[[readRequests]].
    reader->read_requests().append(read_request);
}

// https://streams.spec.whatwg.org/#readable-stream-cancel
GC::Ref<WebIDL::Promise> readable_stream_cancel(ReadableStream& stream, JS::Value reason)
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
    if (reader.has_value()) {
        if (auto* byob_reader = reader->get_pointer<GC::Ref<ReadableStreamBYOBReader>>()) {
            // 1. Let readIntoRequests be reader.[[readIntoRequests]].
            // 2. Set reader.[[readIntoRequests]] to an empty list.
            auto read_into_requests = move((*byob_reader)->read_into_requests());

            // 3. For each readIntoRequest of readIntoRequests,
            for (auto read_into_request : read_into_requests) {
                // 1. Perform readIntoRequest’s close steps, given undefined.
                read_into_request->on_close(JS::js_undefined());
            }
        }
    }

    // 7. Let sourceCancelPromise be ! stream.[[controller]].[[CancelSteps]](reason).
    auto source_cancel_promise = stream.controller()->visit([&](auto controller) {
        return controller->cancel_steps(reason);
    });

    // 8. Return the result of reacting to sourceCancelPromise with a fulfillment step that returns undefined.
    return WebIDL::upon_fulfillment(source_cancel_promise,
        GC::create_function(stream.heap(), [](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            return JS::js_undefined();
        }));
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
    WebIDL::resolve_promise(realm, *reader->visit([](auto reader) {
        return reader->closed_promise_capability();
    }));

    // 6. If reader implements ReadableStreamDefaultReader,
    if (auto* default_reader = reader->get_pointer<GC::Ref<ReadableStreamDefaultReader>>()) {
        // 1. Let readRequests be reader.[[readRequests]].
        // 2. Set reader.[[readRequests]] to an empty list.
        auto read_requests = move((*default_reader)->read_requests());

        // 3. For each readRequest of readRequests,
        for (auto read_request : read_requests) {
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

    auto closed_promise_capability = reader->visit([](auto reader) { return reader->closed_promise_capability(); });

    // 6. Reject reader.[[closedPromise]] with e.
    WebIDL::reject_promise(realm, *closed_promise_capability, error);

    // 7. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*closed_promise_capability);

    reader->visit(
        // 8. If reader implements ReadableStreamDefaultReader,
        [&](GC::Ref<ReadableStreamDefaultReader> reader) {
            // 1. Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e).
            readable_stream_default_reader_error_read_requests(reader, error);
        },
        // 9. Otherwise,
        [&](GC::Ref<ReadableStreamBYOBReader> reader) {
            // 1. Assert: reader implements ReadableStreamBYOBReader.
            // 2. Perform ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
            readable_stream_byob_reader_error_read_into_requests(reader, error);
        });
}

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-into-request
void readable_stream_fulfill_read_into_request(ReadableStream& stream, JS::Value chunk, bool done)
{
    // 1. Assert: ! ReadableStreamHasBYOBReader(stream) is true.
    VERIFY(readable_stream_has_byob_reader(stream));

    // 2. Let reader be stream.[[reader]].
    auto reader = stream.reader()->get<GC::Ref<ReadableStreamBYOBReader>>();

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
    auto reader = stream.reader()->get<GC::Ref<ReadableStreamDefaultReader>>();

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
    return stream.reader()->get<GC::Ref<ReadableStreamBYOBReader>>()->read_into_requests().size();
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-requests
size_t readable_stream_get_num_read_requests(ReadableStream const& stream)
{
    // 1. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
    VERIFY(readable_stream_has_default_reader(stream));

    // 2. Return stream.[[reader]].[[readRequests]]'s size.
    return stream.reader()->get<GC::Ref<ReadableStreamDefaultReader>>()->read_requests().size();
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
    if (reader->has<GC::Ref<ReadableStreamBYOBReader>>())
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
    if (reader->has<GC::Ref<ReadableStreamDefaultReader>>())
        return true;

    // 4. Return false.
    return false;
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-cancel
GC::Ref<WebIDL::Promise> readable_stream_reader_generic_cancel(ReadableStreamGenericReaderMixin& reader, JS::Value reason)
{
    // 1. Let stream be reader.[[stream]]
    auto stream = reader.stream();

    // 2. Assert: stream is not undefined
    VERIFY(stream);

    // 3. Return ! ReadableStreamCancel(stream, reason)
    return readable_stream_cancel(*stream, reason);
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-initialize
void readable_stream_reader_generic_initialize(ReadableStreamReader const& reader, ReadableStream& stream)
{
    auto& mixin = reader.visit([&](auto reader) -> ReadableStreamGenericReaderMixin& { return *reader; });

    // FIXME: Exactly when we should effectively be using the relevant realm of `this` is to be clarified by the spec.
    //        For now, we do so as needed by WPT tests. See: https://github.com/whatwg/streams/issues/1213
    auto& realm = HTML::relevant_realm(reader.visit([](auto reader) -> JS::Object& { return reader; }));

    // 1. Set reader.[[stream]] to stream.
    mixin.set_stream(stream);

    // 2. Set stream.[[reader]] to reader.
    stream.set_reader(reader);

    // 3. If stream.[[state]] is "readable",
    if (auto state = stream.state(); state == ReadableStream::State::Readable) {
        // 1. Set reader.[[closedPromise]] to a new promise.
        mixin.set_closed_promise_capability(WebIDL::create_promise(realm));
    }
    // 4. Otherwise, if stream.[[state]] is "closed",
    else if (state == ReadableStream::State::Closed) {
        // 1. Set reader.[[closedPromise]] to a promise resolved with undefined.
        mixin.set_closed_promise_capability(WebIDL::create_resolved_promise(realm, JS::js_undefined()));
    }
    // 5. Otherwise,
    else {
        // 1. Assert: stream.[[state]] is "errored".
        VERIFY(state == ReadableStream::State::Errored);

        // 2. Set reader.[[closedPromise]] to a promise rejected with stream.[[storedError]].
        mixin.set_closed_promise_capability(WebIDL::create_rejected_promise(realm, stream.stored_error()));

        // 3. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
        WebIDL::mark_promise_as_handled(*mixin.closed_promise_capability());
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
    auto exception = JS::TypeError::create(realm, "Reader has been released"sv);

    // 4. If stream.[[state]] is "readable", reject reader.[[closedPromise]] with a TypeError exception.
    if (stream->state() == ReadableStream::State::Readable) {
        WebIDL::reject_promise(realm, *reader.closed_promise_capability(), exception);
    }
    // 5. Otherwise, set reader.[[closedPromise]] to a promise rejected with a TypeError exception.
    else {
        reader.set_closed_promise_capability(WebIDL::create_rejected_promise(realm, exception));
    }

    // 6. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
    WebIDL::mark_promise_as_handled(*reader.closed_promise_capability());

    // 7. Perform ! stream.[[controller]].[[ReleaseSteps]]().
    stream->controller()->visit([](auto controller) { controller->release_steps(); });

    // 8. Set stream.[[reader]] to undefined.
    stream->set_reader({});

    // 9. Set reader.[[stream]] to undefined.
    reader.set_stream({});
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreadererrorreadintorequests
void readable_stream_byob_reader_error_read_into_requests(ReadableStreamBYOBReader& reader, JS::Value error)
{
    // 1. Let readIntoRequests be reader.[[readIntoRequests]].
    // 2. Set reader.[[readIntoRequests]] to a new empty list.
    auto read_into_requests = move(reader.read_into_requests());

    // 3. For each readIntoRequest of readIntoRequests,
    for (auto read_into_request : read_into_requests) {
        // 1. Perform readIntoRequest’s error steps, given e.
        read_into_request->on_error(error);
    }
}

// https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
void readable_stream_byob_reader_read(ReadableStreamBYOBReader& reader, WebIDL::ArrayBufferView& view, u64 min, ReadIntoRequest& read_into_request)
{
    // 1. Let stream be reader.[[stream]].
    auto stream = reader.stream();

    // 2. Assert: stream is not undefined.
    VERIFY(stream);

    // 3. Set stream.[[disturbed]] to true.
    stream->set_disturbed(true);

    // 4. If stream.[[state]] is "errored", perform readIntoRequest’s error steps given stream.[[storedError]].
    if (stream->state() == ReadableStream::State::Errored) {
        read_into_request.on_error(stream->stored_error());
    }
    // 5. Otherwise, perform ! ReadableByteStreamControllerPullInto(stream.[[controller]], view, min, readIntoRequest).
    else {
        readable_byte_stream_controller_pull_into(stream->controller()->get<GC::Ref<ReadableByteStreamController>>(), view, min, read_into_request);
    }
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

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreadererrorreadrequests
void readable_stream_default_reader_error_read_requests(ReadableStreamDefaultReader& reader, JS::Value error)
{
    // 1. Let readRequests be reader.[[readRequests]].
    // 2. Set reader.[[readRequests]] to a new empty list.
    auto read_requests = move(reader.read_requests());

    // 3. For each readRequest of readRequests,
    for (auto read_request : read_requests) {
        // 1. Perform readRequest’s error steps, given e.
        read_request->on_error(error);
    }
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
    if (auto state = stream->state(); state == ReadableStream::State::Closed) {
        read_request.on_close();
    }
    // 5. Otherwise, if stream.[[state]] is "errored", perform readRequest’s error steps given stream.[[storedError]].
    else if (state == ReadableStream::State::Errored) {
        read_request.on_error(stream->stored_error());
    }
    // 6. Otherwise,
    else {
        // 1. Assert: stream.[[state]] is "readable".
        VERIFY(state == ReadableStream::State::Readable);

        // 2. Perform ! stream.[[controller]].[[PullSteps]](readRequest).
        stream->controller()->visit([&](auto const& controller) {
            return controller->pull_steps(read_request);
        });
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

// https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
WebIDL::ExceptionOr<void> set_up_readable_stream_byob_reader(ReadableStreamBYOBReader& reader, ReadableStream& stream)
{
    // 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError exception.
    if (is_readable_stream_locked(stream))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create stream reader for a locked stream"sv };

    // 2. If stream.[[controller]] does not implement ReadableByteStreamController, throw a TypeError exception.
    if (!stream.controller()->has<GC::Ref<ReadableByteStreamController>>())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "BYOB reader cannot set up reader from non-byte stream"sv };

    // 3. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
    readable_stream_reader_generic_initialize({ reader }, stream);

    // 4. Set reader.[[readIntoRequests]] to a new empty list.
    reader.read_into_requests().clear();

    return {};
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-reader
WebIDL::ExceptionOr<void> set_up_readable_stream_default_reader(ReadableStreamDefaultReader& reader, ReadableStream& stream)
{
    // 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError exception.
    if (is_readable_stream_locked(stream))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create stream reader for a locked stream"sv };

    // 2. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
    readable_stream_reader_generic_initialize({ reader }, stream);

    // 3. Set reader.[[readRequests]] to a new empty list.
    reader.read_requests().clear();

    return {};
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-call-pull-if-needed
void readable_stream_default_controller_call_pull_if_needed(ReadableStreamDefaultController& controller)
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

    WebIDL::react_to_promise(pull_promise,
        // 7. Upon fulfillment of pullPromise,
        GC::create_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Set controller.[[pulling]] to false.
            controller.set_pulling(false);

            // 2. If controller.[[pullAgain]] is true,
            if (controller.pull_again()) {
                // 1. Set controller.[[pullAgain]] to false.
                controller.set_pull_again(false);

                // 2. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
                readable_stream_default_controller_call_pull_if_needed(controller);
            }

            return JS::js_undefined();
        }),

        // 8. Upon rejection of pullPromise with reason e,
        GC::create_function(controller.heap(), [&controller](JS::Value error) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! ReadableStreamDefaultControllerError(controller, e).
            readable_stream_default_controller_error(controller, error);

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
    if (*desired_size > 0.0)
        return true;

    // 8. Return false.
    return false;
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
            readable_stream_default_controller_error(controller, result.value());

            // 2. Return result.
            return result;
        }

        // 3. Let chunkSize be result.[[Value]].
        auto chunk_size = result.release_value();

        // 4. Let enqueueResult be EnqueueValueWithSize(controller, chunk, chunkSize).
        auto enqueue_result = enqueue_value_with_size(controller, chunk, chunk_size);

        // 5. If enqueueResult is an abrupt completion,
        if (enqueue_result.is_error()) {
            auto throw_completion = Bindings::throw_dom_exception_if_needed(vm, [&] { return enqueue_result; }).throw_completion();

            // 1. Perform ! ReadableStreamDefaultControllerError(controller, enqueueResult.[[Value]]).
            readable_stream_default_controller_error(controller, throw_completion.value());

            // 2. Return enqueueResult.
            return throw_completion;
        }
    }

    // 5. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
    readable_stream_default_controller_call_pull_if_needed(controller);

    return {};
}

// https://streams.spec.whatwg.org/#readable-stream-default-controller-error
void readable_stream_default_controller_error(ReadableStreamDefaultController& controller, JS::Value error)
{
    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If stream.[[state]] is not "readable", return.
    if (stream->state() != ReadableStream::State::Readable)
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
    // 1. Let state be controller.[[stream]].[[state]].
    auto state = controller.stream()->state();

    // 2. If state is "errored", return null.
    if (state == ReadableStream::State::Errored)
        return {};

    // 3. If state is "closed", return 0.
    if (state == ReadableStream::State::Closed)
        return 0.0;

    // 4. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
    return controller.strategy_hwm() - controller.queue_total_size();
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

// https://streams.spec.whatwg.org/#readable-stream-default-controller-can-close-or-enqueue
bool readable_stream_default_controller_can_close_or_enqueue(ReadableStreamDefaultController& controller)
{
    // 1. Let state be controller.[[stream]].[[state]].
    auto state = controller.stream()->state();

    // 2. If controller.[[closeRequested]] is false and state is "readable", return true.
    if (!controller.close_requested() && state == ReadableStream::State::Readable)
        return true;

    // 3. Otherwise, return false.
    return false;
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller(ReadableStream& stream, ReadableStreamDefaultController& controller, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<PullAlgorithm> pull_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm, double high_water_mark, GC::Ref<SizeAlgorithm> size_algorithm)
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

    WebIDL::react_to_promise(start_promise,
        // 11. Upon fulfillment of startPromise,
        GC::create_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Set controller.[[started]] to true.
            controller.set_started(true);

            // 2. Assert: controller.[[pulling]] is false.
            VERIFY(!controller.pulling());

            // 3. Assert: controller.[[pullAgain]] is false.
            VERIFY(!controller.pull_again());

            // 4. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(controller).
            readable_stream_default_controller_call_pull_if_needed(controller);

            return JS::js_undefined();
        }),

        // 12. Upon rejection of startPromise with reason r,
        GC::create_function(controller.heap(), [&controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! ReadableStreamDefaultControllerError(controller, r).
            readable_stream_default_controller_error(controller, reason);

            return JS::js_undefined();
        }));

    return {};
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-default-controller-from-underlying-source
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller_from_underlying_source(ReadableStream& stream, JS::Value underlying_source_value, UnderlyingSource const& underlying_source, double high_water_mark, GC::Ref<SizeAlgorithm> size_algorithm)
{
    auto& realm = stream.realm();

    // 1. Let controller be a new ReadableStreamDefaultController.
    auto controller = realm.create<ReadableStreamDefaultController>(realm);

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 3. Let pullAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto pull_algorithm = GC::create_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let cancelAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto cancel_algorithm = GC::create_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. If underlyingSourceDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSourceDict["start"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source.start) {
        start_algorithm = GC::create_function(realm.heap(), [controller, underlying_source_value, callback = underlying_source.start]() -> WebIDL::ExceptionOr<JS::Value> {
            return TRY(WebIDL::invoke_callback(*callback, underlying_source_value, { { controller } }));
        });
    }

    // 6. If underlyingSourceDict["pull"] exists, then set pullAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSourceDict["pull"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source.pull) {
        pull_algorithm = GC::create_function(realm.heap(), [controller, underlying_source_value, callback = underlying_source.pull]() {
            return WebIDL::invoke_promise_callback(*callback, underlying_source_value, { { controller } });
        });
    }

    // 7. If underlyingSourceDict["cancel"] exists, then set cancelAlgorithm to an algorithm which takes an argument
    //    reason and returns the result of invoking underlyingSourceDict["cancel"] with argument list « reason » and
    //    callback this value underlyingSource.
    if (underlying_source.cancel) {
        cancel_algorithm = GC::create_function(realm.heap(), [underlying_source_value, callback = underlying_source.cancel](JS::Value reason) {
            return WebIDL::invoke_promise_callback(*callback, underlying_source_value, { { reason } });
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

    WebIDL::react_to_promise(pull_promise,
        // 7. Upon fulfillment of pullPromise,
        GC::create_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
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
        }),

        // 8. Upon rejection of pullPromise with reason e,
        GC::create_function(controller.heap(), [&controller](JS::Value error) -> WebIDL::ExceptionOr<JS::Value> {
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
        auto first_pending_pull_into = controller.pending_pull_intos().first();

        // 2. If the remainder after dividing firstPendingPullInto’s bytes filled by firstPendingPullInto’s element size is not 0,
        if (first_pending_pull_into->bytes_filled % first_pending_pull_into->element_size != 0) {
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

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-commit-pull-into-descriptor
void readable_byte_stream_controller_commit_pull_into_descriptor(ReadableStream& stream, PullIntoDescriptor const& pull_into_descriptor)
{
    // 1. Assert: stream.[[state]] is not "errored".
    VERIFY(stream.state() != ReadableStream::State::Errored);

    // 2. Assert: pullIntoDescriptor.reader type is not "none".
    VERIFY(pull_into_descriptor.reader_type != ReaderType::None);

    // 3. Let done be false.
    bool done = false;

    // 4. If stream.[[state]] is "closed",
    if (stream.state() == ReadableStream::State::Closed) {
        // 1. Assert: the remainder after dividing pullIntoDescriptor’s bytes filled by pullIntoDescriptor’s element size is 0.
        VERIFY(pull_into_descriptor.bytes_filled % pull_into_descriptor.element_size == 0);

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

    // 4. Assert: the remainder after dividing bytesFilled by elementSize is 0.
    VERIFY(bytes_filled % element_size == 0);

    // 5. Let buffer be ! TransferArrayBuffer(pullIntoDescriptor’s buffer).
    auto buffer = MUST(transfer_array_buffer(realm, pull_into_descriptor.buffer));

    // 6. Return ! Construct(pullIntoDescriptor’s view constructor, « buffer, pullIntoDescriptor’s byte offset, bytesFilled ÷ elementSize »).
    return MUST(JS::construct(vm, *pull_into_descriptor.view_constructor, buffer, JS::Value(pull_into_descriptor.byte_offset), JS::Value(bytes_filled / element_size)));
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue(ReadableByteStreamController& controller, JS::Value chunk)
{
    auto& realm = controller.realm();
    auto& vm = realm.vm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. If controller.[[closeRequested]] is true or stream.[[state]] is not "readable", return.
    if (controller.close_requested() || stream->state() != ReadableStream::State::Readable)
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
        auto first_pending_pull_into = controller.pending_pull_intos().first();

        // 2. If ! IsDetachedBuffer(firstPendingPullInto’s buffer) is true, throw a TypeError exception.
        if (first_pending_pull_into->buffer->is_detached())
            return vm.throw_completion<JS::TypeError>("Buffer is detached"sv);

        // 3. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
        readable_byte_stream_controller_invalidate_byob_request(controller);

        // 4. Set firstPendingPullInto’s buffer to ! TransferArrayBuffer(firstPendingPullInto’s buffer).
        first_pending_pull_into->buffer = MUST(transfer_array_buffer(realm, first_pending_pull_into->buffer));

        // 5. If firstPendingPullInto’s reader type is "none", perform ? ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, firstPendingPullInto).
        if (first_pending_pull_into->reader_type == ReaderType::None)
            TRY(readable_byte_stream_controller_enqueue_detached_pull_into_to_queue(controller, first_pending_pull_into));
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
                VERIFY(controller.pending_pull_intos().first()->reader_type == ReaderType::Default);

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

        // 2. Let filledPullIntos be the result of performing ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
        auto filled_pull_intos = readable_byte_stream_controller_process_pull_into_descriptors_using_queue(controller);

        // 3. For each filledPullInto of filledPullIntos,
        for (auto& filled_pull_into : filled_pull_intos) {
            // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(stream, filledPullInto).
            readable_byte_stream_controller_commit_pull_into_descriptor(*stream, *filled_pull_into);
        }
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

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue-chunk-to-queue
void readable_byte_stream_controller_enqueue_chunk_to_queue(ReadableByteStreamController& controller, GC::Ref<JS::ArrayBuffer> buffer, u32 byte_offset, u32 byte_length)
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
        readable_byte_stream_controller_error(controller, throw_completion.value());

        // 2. Return cloneResult.
        return throw_completion;
    }

    // 3. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller, cloneResult.[[Value]], 0, byteLength).
    readable_byte_stream_controller_enqueue_chunk_to_queue(controller, *clone_result.release_value(), 0, byte_length);

    return {};
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueuedetachedpullintotoqueue
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue_detached_pull_into_to_queue(ReadableByteStreamController& controller, PullIntoDescriptor& pull_into_descriptor)
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

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-head-pull-into-descriptor
void readable_byte_stream_controller_fill_head_pull_into_descriptor(ReadableByteStreamController const& controller, u64 size, PullIntoDescriptor& pull_into_descriptor)
{
    // 1. Assert: either controller.[[pendingPullIntos]] is empty, or controller.[[pendingPullIntos]][0] is pullIntoDescriptor.
    VERIFY(controller.pending_pull_intos().is_empty() || controller.pending_pull_intos().first().ptr() == &pull_into_descriptor);

    // 2. Assert: controller.[[byobRequest]] is null.
    VERIFY(!controller.raw_byob_request());

    // 3. Set pullIntoDescriptor’s bytes filled to bytes filled + size.
    pull_into_descriptor.bytes_filled += size;
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-pull-into-descriptor-from-queue
bool readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(ReadableByteStreamController& controller, PullIntoDescriptor& pull_into_descriptor)
{
    // 1. Let maxBytesToCopy be min(controller.[[queueTotalSize]], pullIntoDescriptor’s byte length − pullIntoDescriptor’s bytes filled).
    auto max_bytes_to_copy = min(controller.queue_total_size(), pull_into_descriptor.byte_length - pull_into_descriptor.bytes_filled);

    // 2. Let maxBytesFilled be pullIntoDescriptor’s bytes filled + maxBytesToCopy.
    u64 max_bytes_filled = pull_into_descriptor.bytes_filled + max_bytes_to_copy;

    // 3. Let totalBytesToCopyRemaining be maxBytesToCopy.
    auto total_bytes_to_copy_remaining = max_bytes_to_copy;

    // 4. Let ready be false.
    bool ready = false;

    // 5. Assert: ! IsDetachedBuffer(pullIntoDescriptor’s buffer) is false.
    VERIFY(!pull_into_descriptor.buffer->is_detached());

    // 6. Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s minimum fill.
    VERIFY(pull_into_descriptor.bytes_filled < pull_into_descriptor.minimum_fill);

    // 7. Let remainderBytes be the remainder after dividing maxBytesFilled by pullIntoDescriptor’s element size.
    auto remainder_bytes = max_bytes_filled % pull_into_descriptor.element_size;

    // 8. Let maxAlignedBytes be maxBytesFilled − remainderBytes.
    auto max_aligned_bytes = max_bytes_filled - remainder_bytes;

    // 9. If maxAlignedBytes ≥ pullIntoDescriptor’s minimum fill,
    if (max_aligned_bytes >= pull_into_descriptor.minimum_fill) {
        // 1. Set totalBytesToCopyRemaining to maxAlignedBytes − pullIntoDescriptor’s bytes filled.
        total_bytes_to_copy_remaining = max_aligned_bytes - pull_into_descriptor.bytes_filled;

        // 2. Set ready to true.
        ready = true;

        // NOTE: A descriptor for a read() request that is not yet filled up to its minimum length will stay at the head
        //       of the queue, so the underlying source can keep filling it.
    }

    // 10. Let queue be controller.[[queue]].
    auto& queue = controller.queue();

    // 11. While totalBytesToCopyRemaining > 0,
    while (total_bytes_to_copy_remaining > 0) {
        // 1. Let headOfQueue be queue[0].
        auto& head_of_queue = queue.first();

        // 2. Let bytesToCopy be min(totalBytesToCopyRemaining, headOfQueue’s byte length).
        auto bytes_to_copy = min(total_bytes_to_copy_remaining, head_of_queue.byte_length);

        // 3. Let destStart be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s bytes filled.
        auto dest_start = pull_into_descriptor.byte_offset + pull_into_descriptor.bytes_filled;

        // 4. Let descriptorBuffer be pullIntoDescriptor’s buffer.
        auto descriptor_buffer = pull_into_descriptor.buffer;

        // 5. Let queueBuffer be headOfQueue’s buffer.
        auto queue_buffer = head_of_queue.buffer;

        // 6. Let queueByteOffset be headOfQueue’s byte offset.
        auto queue_byte_offset = head_of_queue.byte_offset;

        // 7. Assert: ! CanCopyDataBlockBytes(descriptorBuffer, destStart, queueBuffer, queueByteOffset, bytesToCopy) is true.
        VERIFY(can_copy_data_block_bytes_buffer(descriptor_buffer, dest_start, queue_buffer, queue_byte_offset, bytes_to_copy));

        // 8. Perform ! CopyDataBlockBytes(pullIntoDescriptor’s buffer.[[ArrayBufferData]], destStart, headOfQueue’s buffer.[[ArrayBufferData]], headOfQueue’s byte offset, bytesToCopy).
        JS::copy_data_block_bytes(pull_into_descriptor.buffer->buffer(), dest_start, head_of_queue.buffer->buffer(), head_of_queue.byte_offset, bytes_to_copy);

        // 9. If headOfQueue’s byte length is bytesToCopy,
        if (head_of_queue.byte_length == bytes_to_copy) {
            // 1. Remove queue[0].
            queue.take_first();
        }
        // 10. Otherwise,
        else {
            // 1. Set headOfQueue’s byte offset to headOfQueue’s byte offset + bytesToCopy.
            head_of_queue.byte_offset += bytes_to_copy;

            // 2. Set headOfQueue’s byte length to headOfQueue’s byte length − bytesToCopy.
            head_of_queue.byte_length -= bytes_to_copy;
        }

        // 11. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] − bytesToCopy.
        controller.set_queue_total_size(controller.queue_total_size() - bytes_to_copy);

        // 12, Perform ! ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller, bytesToCopy, pullIntoDescriptor).
        readable_byte_stream_controller_fill_head_pull_into_descriptor(controller, bytes_to_copy, pull_into_descriptor);

        // 13. Set totalBytesToCopyRemaining to totalBytesToCopyRemaining − bytesToCopy.
        total_bytes_to_copy_remaining -= bytes_to_copy;
    }

    // 12. If ready is false,
    if (!ready) {
        // 1. Assert: controller.[[queueTotalSize]] is 0.
        VERIFY(controller.queue_total_size() == 0);

        // 2. Assert: pullIntoDescriptor’s bytes filled > 0.
        VERIFY(pull_into_descriptor.bytes_filled > 0);

        // 3. Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s minimum fill.
        VERIFY(pull_into_descriptor.bytes_filled < pull_into_descriptor.minimum_fill);
    }

    // 13. Return ready.
    return ready;
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerfillreadrequestfromqueue
void readable_byte_stream_controller_fill_read_request_from_queue(ReadableByteStreamController& controller, ReadRequest& read_request)
{
    auto& realm = controller.realm();
    auto& vm = realm.vm();

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
    read_request.on_chunk(view);
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollergetbyobrequest
GC::Ptr<ReadableStreamBYOBRequest> readable_byte_stream_controller_get_byob_request(ReadableByteStreamController& controller)
{
    auto& realm = controller.realm();
    auto& vm = realm.vm();

    // 1. If controller.[[byobRequest]] is null and controller.[[pendingPullIntos]] is not empty,
    if (!controller.raw_byob_request() && !controller.pending_pull_intos().is_empty()) {
        // 1. Let firstDescriptor be controller.[[pendingPullIntos]][0].
        auto first_descriptor = controller.pending_pull_intos().first();

        // 2. Let view be ! Construct(%Uint8Array%, « firstDescriptor’s buffer, firstDescriptor’s byte offset + firstDescriptor’s bytes filled, firstDescriptor’s byte length − firstDescriptor’s bytes filled »).
        auto view = MUST(JS::construct(vm, *realm.intrinsics().uint8_array_constructor(), first_descriptor->buffer, JS::Value(first_descriptor->byte_offset + first_descriptor->bytes_filled), JS::Value(first_descriptor->byte_length - first_descriptor->bytes_filled)));

        // 3. Let byobRequest be a new ReadableStreamBYOBRequest.
        auto byob_request = realm.create<ReadableStreamBYOBRequest>(realm);

        // 4. Set byobRequest.[[controller]] to controller.
        byob_request->set_controller(controller);

        // 5. Set byobRequest.[[view]] to view.
        byob_request->set_view(realm.create<WebIDL::ArrayBufferView>(view));

        // 6. Set controller.[[byobRequest]] to byobRequest.
        controller.set_byob_request(byob_request);
    }

    // 2. Return controller.[[byobRequest]].
    return controller.raw_byob_request();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-get-desired-size
Optional<double> readable_byte_stream_controller_get_desired_size(ReadableByteStreamController const& controller)
{
    // 1. Let state be controller.[[stream]].[[state]].
    auto state = controller.stream()->state();

    // 2. If state is "errored", return null.
    if (state == ReadableStream::State::Errored)
        return {};

    // 3. If state is "closed", return 0.
    if (state == ReadableStream::State::Closed)
        return 0.0;

    // 4. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
    return controller.strategy_hwm() - controller.queue_total_size();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-handle-queue-drain
void readable_byte_stream_controller_handle_queue_drain(ReadableByteStreamController& controller)
{
    auto stream = controller.stream();

    // 1. Assert: controller.[[stream]].[[state]] is "readable".
    VERIFY(stream->state() == ReadableStream::State::Readable);

    // 2. If controller.[[queueTotalSize]] is 0 and controller.[[closeRequested]] is true,
    if (controller.queue_total_size() == 0.0 && controller.close_requested()) {
        // 1. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
        readable_byte_stream_controller_clear_algorithms(controller);

        // 2. Perform ! ReadableStreamClose(controller.[[stream]]).
        readable_stream_close(*stream);
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

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-process-pull-into-descriptors-using-queue
SinglyLinkedList<GC::Root<PullIntoDescriptor>> readable_byte_stream_controller_process_pull_into_descriptors_using_queue(ReadableByteStreamController& controller)
{
    // 1. Assert: controller.[[closeRequested]] is false.
    VERIFY(!controller.close_requested());

    // 2. Let filledPullIntos be a new empty list.
    SinglyLinkedList<GC::Root<PullIntoDescriptor>> filled_pull_intos;

    // 3. While controller.[[pendingPullIntos]] is not empty,
    while (!controller.pending_pull_intos().is_empty()) {
        // 1. If controller.[[queueTotalSize]] is 0, then break.
        if (controller.queue_total_size() == 0)
            break;

        // 2. Let pullIntoDescriptor be controller.[[pendingPullIntos]][0].
        auto pull_into_descriptor = controller.pending_pull_intos().first();

        // 3. If ! ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, pullIntoDescriptor) is true,
        if (readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(controller, pull_into_descriptor)) {
            // 1. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
            readable_byte_stream_controller_shift_pending_pull_into(controller);

            // 2. Append pullIntoDescriptor to filledPullIntos.
            filled_pull_intos.append(pull_into_descriptor);
        }
    }

    // 4. Return filledPullIntos.
    return filled_pull_intos;
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerprocessreadrequestsusingqueue
void readable_byte_stream_controller_process_read_requests_using_queue(ReadableByteStreamController& controller)
{
    // 1. Let reader be controller.[[stream]].[[reader]].
    // 2. Assert: reader implements ReadableStreamDefaultReader.
    auto reader = controller.stream()->reader()->get<GC::Ref<ReadableStreamDefaultReader>>();

    // 3. While reader.[[readRequests]] is not empty,
    while (!reader->read_requests().is_empty()) {
        // 1. If controller.[[queueTotalSize]] is 0, return.
        if (controller.queue_total_size() == 0.0)
            return;

        // 2. Let readRequest be reader.[[readRequests]][0].
        // 3. Remove readRequest from reader.[[readRequests]].
        auto read_request = reader->read_requests().take_first();

        // 4. Perform ! ReadableByteStreamControllerFillReadRequestFromQueue(controller, readRequest).
        readable_byte_stream_controller_fill_read_request_from_queue(controller, read_request);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-pull-into
void readable_byte_stream_controller_pull_into(ReadableByteStreamController& controller, WebIDL::ArrayBufferView& view, u64 min, ReadIntoRequest& read_into_request)
{
    auto& realm = controller.realm();
    auto& vm = realm.vm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Let elementSize be 1.
    size_t element_size = 1;

    // 3. Let ctor be %DataView%.
    GC::Ref<JS::NativeFunction> ctor = realm.intrinsics().data_view_constructor();

    // 4. If view has a [[TypedArrayName]] internal slot (i.e., it is not a DataView),
    if (auto const* typed_array = view.bufferable_object().get_pointer<GC::Ref<JS::TypedArrayBase>>()) {
        // 1. Set elementSize to the element size specified in the typed array constructors table for view.[[TypedArrayName]].
        element_size = (*typed_array)->element_size();

        // 2. Set ctor to the constructor specified in the typed array constructors table for view.[[TypedArrayName]].
        switch ((*typed_array)->kind()) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case JS::TypedArrayBase::Kind::ClassName:                                       \
        ctor = realm.intrinsics().snake_name##_constructor();                       \
        break;
            JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
        }
    }

    // 5. Let minimumFill be min × elementSize.
    u64 minimum_fill = min * element_size;

    // 6. Assert: minimumFill ≥ 0 and minimumFill ≤ view.[[ByteLength]].
    VERIFY(minimum_fill <= view.byte_length());

    // 7. Assert: the remainder after dividing minimumFill by elementSize is 0.
    VERIFY(minimum_fill % element_size == 0);

    // 8. Let byteOffset be view.[[ByteOffset]].
    auto byte_offset = view.byte_offset();

    // 6. Let byteLength be view.[[ByteLength]].
    auto byte_length = view.byte_length();

    // 7. Let bufferResult be TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
    auto buffer_result = transfer_array_buffer(realm, *view.viewed_array_buffer());

    // 8. If bufferResult is an abrupt completion,
    if (buffer_result.is_exception()) {
        // 1. Perform readIntoRequest’s error steps, given bufferResult.[[Value]].
        auto throw_completion = Bindings::exception_to_throw_completion(vm, buffer_result.exception());
        read_into_request.on_error(throw_completion.release_value());

        // 2. Return.
        return;
    }

    // 9. Let buffer be bufferResult.[[Value]].
    auto buffer = buffer_result.value();

    // 10. Let pullIntoDescriptor be a new pull-into descriptor with
    //
    //     buffer                   buffer
    //     buffer byte length       buffer.[[ArrayBufferByteLength]]
    //     byte offset              byteOffset
    //     byte length              byteLength
    //     bytes filled             0
    //     minimum fill             minimumFill
    //     element size             elementSize
    //     view constructor         ctor
    //     reader type              "byob"
    auto pull_into_descriptor = vm.heap().allocate<PullIntoDescriptor>(
        buffer,
        buffer->byte_length(),
        byte_offset,
        byte_length,
        0,
        minimum_fill,
        element_size,
        *ctor,
        ReaderType::Byob);

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
    if (stream->state() == ReadableStream::State::Closed) {
        // 1. Let emptyView be ! Construct(ctor, « pullIntoDescriptor’s buffer, pullIntoDescriptor’s byte offset, 0 »).
        auto empty_view = MUST(JS::construct(vm, *ctor, pull_into_descriptor->buffer, JS::Value(pull_into_descriptor->byte_offset), JS::Value(0)));

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

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond(ReadableByteStreamController& controller, u64 bytes_written)
{
    auto& realm = controller.realm();

    // 1. Assert: controller.[[pendingPullIntos]] is not empty.
    VERIFY(!controller.pending_pull_intos().is_empty());

    // 2. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    auto first_descriptor = controller.pending_pull_intos().first();

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
        if (first_descriptor->bytes_filled + bytes_written > first_descriptor->byte_length)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Bytes written is greater than the pull requests byte length"sv };
    }

    // 6. Set firstDescriptor’s buffer to ! TransferArrayBuffer(firstDescriptor’s buffer).
    first_descriptor->buffer = MUST(transfer_array_buffer(realm, *first_descriptor->buffer));

    // 7. Perform ? ReadableByteStreamControllerRespondInternal(controller, bytesWritten).
    return readable_byte_stream_controller_respond_internal(controller, bytes_written);
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
    auto stream = controller.stream();

    // 4. If ! ReadableStreamHasBYOBReader(stream) is true,
    if (readable_stream_has_byob_reader(*stream)) {
        // 1. Let filledPullIntos be a new empty list.
        SinglyLinkedList<GC::Root<PullIntoDescriptor>> filled_pull_intos;

        // 2. While filledPullIntos’s size < ! ReadableStreamGetNumReadIntoRequests(stream),
        while (filled_pull_intos.size() < readable_stream_get_num_read_into_requests(*stream)) {
            // 1. Let pullIntoDescriptor be ! ReadableByteStreamControllerShiftPendingPullInto(controller).
            auto pull_into_descriptor = readable_byte_stream_controller_shift_pending_pull_into(controller);

            // 2. Append pullIntoDescriptor to filledPullIntos.
            filled_pull_intos.append(pull_into_descriptor);
        }

        // 3. For each filledPullInto of filledPullIntos,
        for (auto& filled_pull_into : filled_pull_intos) {
            // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(stream, filledPullInto).
            readable_byte_stream_controller_commit_pull_into_descriptor(*stream, *filled_pull_into);
        }
    }
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
        TRY(readable_byte_stream_controller_enqueue_detached_pull_into_to_queue(controller, pull_into_descriptor));

        // 2. Let filledPullIntos be the result of performing ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
        auto filled_pulled_intos = readable_byte_stream_controller_process_pull_into_descriptors_using_queue(controller);

        // 3. For each filledPullInto of filledPullIntos,
        for (auto& filled_pull_into : filled_pulled_intos) {
            // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], filledPullInto).
            readable_byte_stream_controller_commit_pull_into_descriptor(*controller.stream(), *filled_pull_into);
        }

        // 4. Return.
        return {};
    }

    // 4. If pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s minimum fill, return.
    if (pull_into_descriptor.bytes_filled < pull_into_descriptor.minimum_fill)
        return {};

    // NOTE: A descriptor for a read() request that is not yet filled up to its minimum length will stay at the head of
    //       the queue, so the underlying source can keep filling it.

    // 5. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
    readable_byte_stream_controller_shift_pending_pull_into(controller);

    // 6. Let remainderSize be the remainder after dividing pullIntoDescriptor’s bytes filled by pullIntoDescriptor’s element size.
    auto remainder_size = pull_into_descriptor.bytes_filled % pull_into_descriptor.element_size;

    // 7. If remainderSize > 0,
    if (remainder_size > 0) {
        // 1. Let end be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s bytes filled.
        auto end = pull_into_descriptor.byte_offset + pull_into_descriptor.bytes_filled;

        // 2. Perform ? ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller, pullIntoDescriptor’s buffer, end − remainderSize, remainderSize).
        TRY(readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(controller, *pull_into_descriptor.buffer, end - remainder_size, remainder_size));
    }

    // 8. Set pullIntoDescriptor’s bytes filled to pullIntoDescriptor’s bytes filled − remainderSize.
    pull_into_descriptor.bytes_filled -= remainder_size;

    // 9. Let filledPullIntos be the result of performing ! ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller).
    auto filled_pulled_intos = readable_byte_stream_controller_process_pull_into_descriptors_using_queue(controller);

    // 10. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], pullIntoDescriptor).
    readable_byte_stream_controller_commit_pull_into_descriptor(*controller.stream(), pull_into_descriptor);

    // 11. For each filledPullInto of filledPullIntos,
    for (auto& filled_pull_into : filled_pulled_intos) {
        // 1. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]], filledPullInto).
        readable_byte_stream_controller_commit_pull_into_descriptor(*controller.stream(), *filled_pull_into);
    }

    return {};
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-internal
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_internal(ReadableByteStreamController& controller, u64 bytes_written)
{
    // 1. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    auto first_descriptor = controller.pending_pull_intos().first();

    // 2. Assert: ! CanTransferArrayBuffer(firstDescriptor’s buffer) is true.
    VERIFY(can_transfer_array_buffer(*first_descriptor->buffer));

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

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-with-new-view
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_with_new_view(JS::Realm& realm, ReadableByteStreamController& controller, WebIDL::ArrayBufferView& view)
{
    // 1. Assert: controller.[[pendingPullIntos]] is not empty.
    VERIFY(!controller.pending_pull_intos().is_empty());

    // 2. Assert: ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is false.
    VERIFY(!view.viewed_array_buffer()->is_detached());

    // 3. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    auto first_descriptor = controller.pending_pull_intos().first();

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
    if (first_descriptor->byte_offset + first_descriptor->bytes_filled != view.byte_offset())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Byte offset is not aligned with the pull request's byte offset"sv };

    // 8. If firstDescriptor’s buffer byte length is not view.[[ViewedArrayBuffer]].[[ByteLength]], throw a RangeError exception.
    if (first_descriptor->buffer_byte_length != view.viewed_array_buffer()->byte_length())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Buffer byte length is not aligned with the pull request's byte length"sv };

    // 9. If firstDescriptor’s bytes filled + view.[[ByteLength]] > firstDescriptor’s byte length, throw a RangeError exception.
    if (first_descriptor->bytes_filled + view.byte_length() > first_descriptor->byte_length)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Byte length is greater than the pull request's byte length"sv };

    // 10. Let viewByteLength be view.[[ByteLength]].
    auto view_byte_length = view.byte_length();

    // 11. Set firstDescriptor’s buffer to ? TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
    first_descriptor->buffer = TRY(transfer_array_buffer(realm, *view.viewed_array_buffer()));

    // 12. Perform ? ReadableByteStreamControllerRespondInternal(controller, viewByteLength).
    TRY(readable_byte_stream_controller_respond_internal(controller, view_byte_length));

    return {};
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-shift-pending-pull-into
GC::Ref<PullIntoDescriptor> readable_byte_stream_controller_shift_pending_pull_into(ReadableByteStreamController& controller)
{
    // 1. Assert: controller.[[byobRequest]] is null.
    VERIFY(!controller.raw_byob_request());

    // 2. Let descriptor be controller.[[pendingPullIntos]][0].
    // 3. Remove descriptor from controller.[[pendingPullIntos]].
    auto descriptor = controller.pending_pull_intos().take_first();

    // 4. Return descriptor.
    return descriptor;
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

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller(ReadableStream& stream, ReadableByteStreamController& controller, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<PullAlgorithm> pull_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm, double high_water_mark, JS::Value auto_allocate_chunk_size)
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

    WebIDL::react_to_promise(start_promise,
        // 16. Upon fulfillment of startPromise,
        GC::create_function(controller.heap(), [&controller](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Set controller.[[started]] to true.
            controller.set_started(true);

            // 2. Assert: controller.[[pulling]] is false.
            VERIFY(!controller.pulling());

            // 3. Assert: controller.[[pullAgain]] is false.
            VERIFY(!controller.pull_again());

            // 4. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
            readable_byte_stream_controller_call_pull_if_needed(controller);

            return JS::js_undefined();
        }),

        // 17. Upon rejection of startPromise with reason r,
        GC::create_function(controller.heap(), [&controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! ReadableByteStreamControllerError(controller, r).
            readable_byte_stream_controller_error(controller, reason);

            return JS::js_undefined();
        }));

    return {};
}

// https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller-from-underlying-source
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller_from_underlying_source(ReadableStream& stream, JS::Value underlying_source, UnderlyingSource const& underlying_source_dict, double high_water_mark)
{
    auto& realm = stream.realm();

    // 1. Let controller be a new ReadableByteStreamController.
    auto controller = realm.create<ReadableByteStreamController>(realm);

    // 2. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 3. Let pullAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto pull_algorithm = GC::create_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 4. Let cancelAlgorithm be an algorithm that returns a promise resolved with undefined.
    auto cancel_algorithm = GC::create_function(realm.heap(), [&realm](JS::Value) {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. If underlyingSourceDict["start"] exists, then set startAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSourceDict["start"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source_dict.start) {
        start_algorithm = GC::create_function(realm.heap(), [controller, underlying_source, callback = underlying_source_dict.start]() -> WebIDL::ExceptionOr<JS::Value> {
            return TRY(WebIDL::invoke_callback(*callback, underlying_source, { { controller } }));
        });
    }

    // 6. If underlyingSourceDict["pull"] exists, then set pullAlgorithm to an algorithm which returns the result of
    //    invoking underlyingSourceDict["pull"] with argument list « controller » and callback this value underlyingSource.
    if (underlying_source_dict.pull) {
        pull_algorithm = GC::create_function(realm.heap(), [controller, underlying_source, callback = underlying_source_dict.pull]() {
            return WebIDL::invoke_promise_callback(*callback, underlying_source, { { controller } });
        });
    }

    // 7. If underlyingSourceDict["cancel"] exists, then set cancelAlgorithm to an algorithm which takes an argument
    //    reason and returns the result of invoking underlyingSourceDict["cancel"] with argument list « reason » and
    //    callback this value underlyingSource.
    if (underlying_source_dict.cancel) {
        cancel_algorithm = GC::create_function(realm.heap(), [underlying_source, callback = underlying_source_dict.cancel](JS::Value reason) {
            return WebIDL::invoke_promise_callback(*callback, underlying_source, { { reason } });
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
