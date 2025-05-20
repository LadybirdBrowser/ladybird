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
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/StructuredSerializeOptions.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultController.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Streams {

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

struct PromiseHolder : public JS::Cell {
    GC_CELL(PromiseHolder, JS::Cell);
    GC_DECLARE_ALLOCATOR(PromiseHolder);

    explicit PromiseHolder(GC::Ptr<WebIDL::Promise> promise)
        : promise(promise)
    {
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(promise);
    }

    GC::Ptr<WebIDL::Promise> promise;
};

GC_DEFINE_ALLOCATOR(PromiseHolder);

static void add_message_event_listener(JS::Realm& realm, HTML::MessagePort& port, FlyString const& name, Function<void(JS::VM&, HTML::MessageEvent const&)> handler)
{
    auto behavior = [handler = GC::create_function(realm.heap(), move(handler))](JS::VM& vm) {
        auto event = vm.argument(0);
        VERIFY(event.is_object());

        auto& message_event = as<HTML::MessageEvent>(event.as_object());
        handler->function()(vm, message_event);

        return JS::js_undefined();
    };

    auto function = JS::NativeFunction::create(realm, move(behavior), 1, FlyString {}, &realm);
    auto callback = realm.heap().allocate<WebIDL::CallbackType>(function, realm);
    auto listener = DOM::IDLEventListener::create(realm, callback);

    port.add_event_listener_without_options(name, listener);
}

// https://streams.spec.whatwg.org/#abstract-opdef-crossrealmtransformsenderror
void cross_realm_transform_send_error(JS::Realm& realm, HTML::MessagePort& port, JS::Value error)
{
    // 1. Perform PackAndPostMessage(port, "error", error), discarding the result.
    (void)pack_and_post_message(realm, port, "error"sv, error);
}

// https://streams.spec.whatwg.org/#abstract-opdef-packandpostmessagehandlingerror
WebIDL::ExceptionOr<void> pack_and_post_message(JS::Realm& realm, HTML::MessagePort& port, StringView type, JS::Value value)
{
    auto& vm = realm.vm();

    // 1. Let message be OrdinaryObjectCreate(null).
    auto message = JS::Object::create(realm, nullptr);

    // 2. Perform ! CreateDataProperty(message, "type", type).
    MUST(message->create_data_property(vm.names.type, JS::PrimitiveString::create(vm, type)));

    // 3. Perform ! CreateDataProperty(message, "value", value).
    MUST(message->create_data_property(vm.names.value, value));

    // 4. Let targetPort be the port with which port is entangled, if any; otherwise let it be null.
    auto target_port = port.entangled_port();

    // 5. Let options be «[ "transfer" → « » ]».
    HTML::StructuredSerializeOptions options { .transfer = {} };

    // 6. Run the message port post message steps providing targetPort, message, and options.
    return port.message_port_post_message_steps(target_port, message, options);
}

// https://streams.spec.whatwg.org/#abstract-opdef-packandpostmessagehandlingerror
WebIDL::ExceptionOr<void> pack_and_post_message_handling_error(JS::Realm& realm, HTML::MessagePort& port, StringView type, JS::Value value)
{
    // 1. Let result be PackAndPostMessage(port, type, value).
    auto result = pack_and_post_message(realm, port, type, value);

    // 2. If result is an abrupt completion,
    if (result.is_exception()) {
        auto error = Bindings::exception_to_throw_completion(realm.vm(), result.release_error());

        // 1. Perform ! CrossRealmTransformSendError(port, result.[[Value]]).
        cross_realm_transform_send_error(realm, port, error.value());
    }

    // 3. Return result as a completion record.
    return result;
}

// https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformreadable
void set_up_cross_realm_transform_readable(JS::Realm& realm, ReadableStream& stream, HTML::MessagePort& port)
{
    // 1. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(stream);

    // 2. Let controller be a new ReadableStreamDefaultController.
    auto controller = realm.create<ReadableStreamDefaultController>(realm);

    // 3. Add a handler for port’s message event with the following steps:
    add_message_event_listener(realm, port, HTML::EventNames::message,
        [&port, controller](JS::VM& vm, HTML::MessageEvent const& message) {
            // 1. Let data be the data of the message.
            auto data = message.data();

            // 2. Assert: data is an Object.
            VERIFY(data.is_object());

            // 3. Let type be ! Get(data, "type").
            auto type = MUST(data.get(vm, vm.names.type));

            // 4. Let value be ! Get(data, "value").
            auto value = MUST(data.get(vm, vm.names.value));

            // 5. Assert: type is a String.
            auto type_string = type.as_string().utf8_string_view();

            // 6. If type is "chunk",
            if (type_string == "chunk"sv) {
                // 1. Perform ! ReadableStreamDefaultControllerEnqueue(controller, value).
                MUST(readable_stream_default_controller_enqueue(controller, value));
            }
            // 7. Otherwise, if type is "close",
            else if (type_string == "close"sv) {
                // 1. Perform ! ReadableStreamDefaultControllerClose(controller).
                readable_stream_default_controller_close(controller);

                // 2. Disentangle port.
                port.disentangle();
            }
            // 8. Otherwise, if type is "error",
            else if (type_string == "error"sv) {
                // 1. Perform ! ReadableStreamDefaultControllerError(controller, value).
                readable_stream_default_controller_error(controller, value);

                // 2. Disentangle port.
                port.disentangle();
            }
        });

    // 4. Add a handler for port’s messageerror event with the following steps:
    add_message_event_listener(realm, port, HTML::EventNames::messageerror,
        [&realm, &port, controller](JS::VM&, HTML::MessageEvent const&) {
            // 1. Let error be a new "DataCloneError" DOMException.
            auto error = WebIDL::DataCloneError::create(realm, "Unable to transfer stream"_string);

            // 2. Perform ! CrossRealmTransformSendError(port, error).
            cross_realm_transform_send_error(realm, port, error);

            // 3. Perform ! ReadableStreamDefaultControllerError(controller, error).
            readable_stream_default_controller_error(controller, error);

            // 4. Disentangle port.
            port.disentangle();
        });

    // FIXME: 5. Enable port’s port message queue.

    // 6. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 7. Let pullAlgorithm be the following steps:
    auto pull_algorithm = GC::create_function(realm.heap(), [&realm, &port]() -> GC::Ref<WebIDL::Promise> {
        // 1. Perform ! PackAndPostMessage(port, "pull", undefined).
        MUST(pack_and_post_message(realm, port, "pull"sv, JS::js_undefined()));

        // 2. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 8. Let cancelAlgorithm be the following steps, taking a reason argument:
    auto cancel_algorithm = GC::create_function(realm.heap(), [&realm, &port](JS::Value reason) -> GC::Ref<WebIDL::Promise> {
        // 1. Let result be PackAndPostMessageHandlingError(port, "error", reason).
        auto result = pack_and_post_message_handling_error(realm, port, "error"sv, reason);

        // 2. Disentangle port.
        port.disentangle();

        // 3. If result is an abrupt completion, return a promise rejected with result.[[Value]].
        if (result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());

        // 4. Otherwise, return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 9. Let sizeAlgorithm be an algorithm that returns 1.
    auto size_algorithm = GC::create_function(realm.heap(), [](JS::Value) -> JS::Completion {
        return JS::Value { 1 };
    });

    // 10. Perform ! SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, 0, sizeAlgorithm).
    MUST(set_up_readable_stream_default_controller(stream, controller, start_algorithm, pull_algorithm, cancel_algorithm, 0, size_algorithm));
}

// https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformwritable
void set_up_cross_realm_transform_writable(JS::Realm& realm, WritableStream& stream, HTML::MessagePort& port)
{
    // 1. Perform ! InitializeWritableStream(stream).
    initialize_writable_stream(stream);

    // 2. Let controller be a new WritableStreamDefaultController.
    auto controller = realm.create<WritableStreamDefaultController>(realm);

    // 3. Let backpressurePromise be a new promise.
    auto backpressure_promise = realm.heap().allocate<PromiseHolder>(WebIDL::create_promise(realm));

    // 4. Add a handler for port’s message event with the following steps:
    add_message_event_listener(realm, port, HTML::EventNames::message,
        [&realm, controller, backpressure_promise](JS::VM& vm, HTML::MessageEvent const& message) {
            // 1. Let data be the data of the message.
            auto data = message.data();

            // 2. Assert: data is an Object.
            VERIFY(data.is_object());

            // 3. Let type be ! Get(data, "type").
            auto type = MUST(data.get(vm, vm.names.type));

            // 4. Let value be ! Get(data, "value").
            auto value = MUST(data.get(vm, vm.names.value));

            // 5. Assert: type is a String.
            auto type_string = type.as_string().utf8_string_view();

            // 6. If type is "pull",
            if (type_string == "pull"sv) {
                // 1. If backpressurePromise is not undefined,
                if (backpressure_promise->promise) {
                    // 1. Resolve backpressurePromise with undefined.
                    WebIDL::resolve_promise(realm, *backpressure_promise->promise, JS::js_undefined());

                    // 2. Set backpressurePromise to undefined.
                    backpressure_promise->promise = nullptr;
                }
            }
            // 7. Otherwise, if type is "error",
            else if (type_string == "error"sv) {
                // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, value).
                writable_stream_default_controller_error_if_needed(controller, value);

                // 2. If backpressurePromise is not undefined,
                if (backpressure_promise->promise) {
                    // 1. Resolve backpressurePromise with undefined.
                    WebIDL::resolve_promise(realm, *backpressure_promise->promise, JS::js_undefined());

                    // 2. Set backpressurePromise to undefined.
                    backpressure_promise->promise = nullptr;
                }
            }
        });

    // 5. Add a handler for port’s messageerror event with the following steps:
    add_message_event_listener(realm, port, HTML::EventNames::messageerror,
        [&realm, &port, controller](JS::VM&, HTML::MessageEvent const&) {
            // 1. Let error be a new "DataCloneError" DOMException
            auto error = WebIDL::DataCloneError::create(realm, "Unable to transfer stream"_string);

            // 2. Perform ! CrossRealmTransformSendError(port, error).
            cross_realm_transform_send_error(realm, port, error);

            // 3. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, error).
            writable_stream_default_controller_error_if_needed(controller, error);

            // 4. Disentangle port.
            port.disentangle();
        });

    // FIXME: 6. Enable port’s port message queue.

    // 7. Let startAlgorithm be an algorithm that returns undefined.
    auto start_algorithm = GC::create_function(realm.heap(), []() -> WebIDL::ExceptionOr<JS::Value> {
        return JS::js_undefined();
    });

    // 8. Let writeAlgorithm be the following steps, taking a chunk argument:
    auto write_algorithm = GC::create_function(realm.heap(), [&realm, &port, backpressure_promise](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        // 1. If backpressurePromise is undefined, set backpressurePromise to a promise resolved with undefined.
        if (!backpressure_promise->promise)
            backpressure_promise->promise = WebIDL::create_resolved_promise(realm, JS::js_undefined());

        // FIXME: The steps below ("Return a promise rejected/resolved with ...") seem to indicate we should be creating
        //        a promise on-the-fly. But in order for the error from PackAndPostMessageHandlingError to be propagated
        //        back to the original ReadableStream, we must actually fulfill the promise created from reacting to the
        //        backpressure promise. This is explicitly tested by WPT.
        auto reaction_promise = realm.heap().allocate<PromiseHolder>(nullptr);

        // 2. Return the result of reacting to backpressurePromise with the following fulfillment steps:
        reaction_promise->promise = WebIDL::upon_fulfillment(*backpressure_promise->promise,
            GC::create_function(realm.heap(), [&realm, &port, backpressure_promise, reaction_promise, chunk](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                // 1. Set backpressurePromise to a new promise.
                backpressure_promise->promise = WebIDL::create_promise(realm);

                // 2. Let result be PackAndPostMessageHandlingError(port, "chunk", chunk).
                auto result = pack_and_post_message_handling_error(realm, port, "chunk"sv, chunk);

                // 3. If result is an abrupt completion,
                if (result.is_error()) {
                    // 1. Disentangle port.
                    port.disentangle();

                    // 2. Return a promise rejected with result.[[Value]].
                    auto error = Bindings::exception_to_throw_completion(realm.vm(), result.release_error());
                    WebIDL::reject_promise(realm, *reaction_promise->promise, error.value());
                }
                // 4. Otherwise, return a promise resolved with undefined.
                else {
                    WebIDL::resolve_promise(realm, *reaction_promise->promise, JS::js_undefined());
                }

                return reaction_promise->promise;
            }));

        return *reaction_promise->promise;
    });

    // 9. Let closeAlgorithm be the folowing steps:
    auto close_algorithm = GC::create_function(realm.heap(), [&realm, &port]() -> GC::Ref<WebIDL::Promise> {
        // 1. Perform ! PackAndPostMessage(port, "close", undefined).
        MUST(pack_and_post_message(realm, port, "close"sv, JS::js_undefined()));

        // 2. Disentangle port.
        port.disentangle();

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 10. Let abortAlgorithm be the following steps, taking a reason argument:
    auto abort_algorithm = GC::create_function(realm.heap(), [&realm, &port](JS::Value reason) -> GC::Ref<WebIDL::Promise> {
        // 1. Let result be PackAndPostMessageHandlingError(port, "error", reason).
        auto result = pack_and_post_message_handling_error(realm, port, "error"sv, reason);

        // 2. Disentangle port.
        port.disentangle();

        // 3. If result is an abrupt completion, return a promise rejected with result.[[Value]].
        if (result.is_error())
            return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());

        // 4. Otherwise, return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 11. Let sizeAlgorithm be an algorithm that returns 1.
    auto size_algorithm = GC::create_function(realm.heap(), [](JS::Value) -> JS::Completion {
        return JS::Value { 1 };
    });

    // 12. Perform ! SetUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, 1, sizeAlgorithm).
    MUST(set_up_writable_stream_default_controller(stream, controller, start_algorithm, write_algorithm, close_algorithm, abort_algorithm, 1, size_algorithm));
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

}
