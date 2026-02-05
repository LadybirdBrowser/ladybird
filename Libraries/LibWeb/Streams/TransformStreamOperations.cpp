/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>
#include <LibWeb/Streams/TransformStreamOperations.h>
#include <LibWeb/Streams/Transformer.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultController.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

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

// https://streams.spec.whatwg.org/#transform-stream-error
void transform_stream_error(TransformStream& stream, JS::Value error)
{
    auto readable_controller = stream.readable()->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

    // 1. Perform ! ReadableStreamDefaultControllerError(stream.[[readable]].[[controller]], e).
    readable_stream_default_controller_error(readable_controller, error);

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
    if (auto backpressure_change_promise = stream.backpressure_change_promise())
        WebIDL::resolve_promise(realm, *backpressure_change_promise, JS::js_undefined());

    // 3. Set stream.[[backpressureChangePromise]] to a new promise.
    stream.set_backpressure_change_promise(WebIDL::create_promise(realm));

    // 4. Set stream.[[backpressure]] to backpressure.
    stream.set_backpressure(backpressure);
}

// https://streams.spec.whatwg.org/#transform-stream-unblock-write
void transform_stream_unblock_write(TransformStream& stream)
{
    // 1. If stream.[[backpressure]] is true, perform ! TransformStreamSetBackpressure(stream, false).
    if (stream.backpressure() == true)
        transform_stream_set_backpressure(stream, false);
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
    auto transform_algorithm = GC::create_function(realm.heap(), [&realm, &vm, controller](JS::Value chunk) {
        // 1. Let result be TransformStreamDefaultControllerEnqueue(controller, chunk).
        auto result = transform_stream_default_controller_enqueue(controller, chunk);

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
        transform_algorithm = GC::create_function(realm.heap(), [transformer, controller, callback = transformer_dict.transform](JS::Value chunk) {
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
    set_up_transform_stream_default_controller(stream, controller, transform_algorithm, flush_algorithm, cancel_algorithm);
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
    auto readable_controller = stream->readable()->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

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

// https://streams.spec.whatwg.org/#transform-stream-default-controller-perform-transform
GC::Ref<WebIDL::Promise> transform_stream_default_controller_perform_transform(TransformStreamDefaultController& controller, JS::Value chunk)
{
    auto& realm = controller.realm();

    // 1. Let transformPromise be the result of performing controller.[[transformAlgorithm]], passing chunk.
    auto transform_promise = controller.transform_algorithm()->function()(chunk);

    // 2. Return the result of reacting to transformPromise with the following rejection steps given the argument r:
    return WebIDL::upon_rejection(*transform_promise,
        GC::create_function(realm.heap(), [&controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Perform ! TransformStreamError(controller.[[stream]], r).
            transform_stream_error(*controller.stream(), reason);

            // 2. Throw r.
            return JS::throw_completion(reason);
        }));
}

// https://streams.spec.whatwg.org/#transform-stream-default-controller-terminate
void transform_stream_default_controller_terminate(TransformStreamDefaultController& controller)
{
    auto& realm = controller.realm();

    // 1. Let stream be controller.[[stream]].
    auto stream = controller.stream();

    // 2. Let readableController be stream.[[readable]].[[controller]].
    auto readable_controller = stream->readable()->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

    // 3. Perform ! ReadableStreamDefaultControllerClose(readableController).
    readable_stream_default_controller_close(readable_controller);

    // 4. Let error be a TypeError exception indicating that the stream has been terminated.
    auto error = JS::TypeError::create(realm, "Stream has been terminated."sv);

    // 5. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, error).
    transform_stream_error_writable_and_unblock_write(*stream, error);
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
    if (stream.backpressure() == true) {
        // 1. Let backpressureChangePromise be stream.[[backpressureChangePromise]].
        auto backpressure_change_promise = stream.backpressure_change_promise();

        // 2. Assert: backpressureChangePromise is not undefined.
        VERIFY(backpressure_change_promise);

        // 3. Return the result of reacting to backpressureChangePromise with the following fulfillment steps:
        return WebIDL::upon_fulfillment(*backpressure_change_promise,
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
            }));
    }

    // 4. Return ! TransformStreamDefaultControllerPerformTransform(controller, chunk).
    return transform_stream_default_controller_perform_transform(*controller, chunk);
}

// https://streams.spec.whatwg.org/#transform-stream-default-sink-abort-algorithm
GC::Ref<WebIDL::Promise> transform_stream_default_sink_abort_algorithm(TransformStream& stream, JS::Value reason)
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
                // 1. Perform ! ReadableStreamDefaultControllerError(readable.[[controller]], reason).
                readable_stream_default_controller_error(readable->controller()->get<GC::Ref<ReadableStreamDefaultController>>(), reason);

                // 2. Resolve controller.[[finishPromise]] with undefined.
                WebIDL::resolve_promise(realm, *controller->finish_promise(), JS::js_undefined());
            }

            return JS::js_undefined();
        }),

        // 2. If cancelPromise was rejected with reason r, then:
        GC::create_function(realm.heap(), [&realm, readable, controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
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
        GC::create_function(realm.heap(), [&realm, &stream, writable, controller, reason](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
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
        GC::create_function(realm.heap(), [&realm, &stream, writable, controller](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
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

// https://streams.spec.whatwg.org/#transform-stream-default-source-pull
GC::Ref<WebIDL::Promise> transform_stream_default_source_pull_algorithm(TransformStream& stream)
{
    // 1. Assert: stream.[[backpressure]] is true.
    VERIFY(stream.backpressure() == true);

    // 2. Assert: stream.[[backpressureChangePromise]] is not undefined.
    VERIFY(stream.backpressure_change_promise());

    // 3. Perform ! TransformStreamSetBackpressure(stream, false).
    transform_stream_set_backpressure(stream, false);

    // 4. Return stream.[[backpressureChangePromise]].
    return *stream.backpressure_change_promise();
}

}
