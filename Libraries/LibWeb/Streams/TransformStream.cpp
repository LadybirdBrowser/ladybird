/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/File.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TransformStreamPrototype.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>
#include <LibWeb/Streams/TransformStreamOperations.h>
#include <LibWeb/Streams/Transformer.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(TransformStream);

// https://streams.spec.whatwg.org/#ts-constructor
WebIDL::ExceptionOr<GC::Ref<TransformStream>> TransformStream::construct_impl(JS::Realm& realm, Optional<GC::Root<JS::Object>> transformer_object, QueuingStrategy const& writable_strategy, QueuingStrategy const& readable_strategy)
{
    auto& vm = realm.vm();

    auto stream = realm.create<TransformStream>(realm);

    // 1. If transformer is missing, set it to null.
    auto transformer = transformer_object.has_value() ? JS::Value { transformer_object.value() } : JS::js_null();

    // 2. Let transformerDict be transformer, converted to an IDL value of type Transformer.
    auto transformer_dict = TRY(Transformer::from_value(vm, transformer));

    // 3. If transformerDict["readableType"] exists, throw a RangeError exception.
    if (transformer_dict.readable_type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Invalid use of reserved key 'readableType'"sv };

    // 4. If transformerDict["writableType"] exists, throw a RangeError exception.
    if (transformer_dict.writable_type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Invalid use of reserved key 'writableType'"sv };

    // 5. Let readableHighWaterMark be ? ExtractHighWaterMark(readableStrategy, 0).
    auto readable_high_water_mark = TRY(extract_high_water_mark(readable_strategy, 0));

    // 6. Let readableSizeAlgorithm be ! ExtractSizeAlgorithm(readableStrategy).
    auto readable_size_algorithm = extract_size_algorithm(vm, readable_strategy);

    // 7. Let writableHighWaterMark be ? ExtractHighWaterMark(writableStrategy, 1).
    auto writable_high_water_mark = TRY(extract_high_water_mark(writable_strategy, 1));

    // 8. Let writableSizeAlgorithm be ! ExtractSizeAlgorithm(writableStrategy).
    auto writable_size_algorithm = extract_size_algorithm(vm, writable_strategy);

    // 9. Let startPromise be a new promise.
    auto start_promise = WebIDL::create_promise(realm);

    // 10. Perform ! InitializeTransformStream(this, startPromise, writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark, readableSizeAlgorithm).
    initialize_transform_stream(*stream, start_promise, writable_high_water_mark, writable_size_algorithm, readable_high_water_mark, readable_size_algorithm);

    // 11. Perform ? SetUpTransformStreamDefaultControllerFromTransformer(this, transformer, transformerDict).
    set_up_transform_stream_default_controller_from_transformer(*stream, transformer, transformer_dict);

    // 12. If transformerDict["start"] exists, then resolve startPromise with the result of invoking
    //     transformerDict["start"] with argument list « this.[[controller]] » and callback this value transformer.
    if (transformer_dict.start) {
        auto result = TRY(WebIDL::invoke_callback(*transformer_dict.start, transformer, { { stream->controller() } }));
        WebIDL::resolve_promise(realm, start_promise, result);
    }
    // 13. Otherwise, resolve startPromise with undefined.
    else {
        WebIDL::resolve_promise(realm, start_promise, JS::js_undefined());
    }

    return stream;
}

TransformStream::TransformStream(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

TransformStream::~TransformStream() = default;

void TransformStream::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TransformStream);
    Base::initialize(realm);
}

void TransformStream::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_backpressure_change_promise);
    visitor.visit(m_controller);
    visitor.visit(m_readable);
    visitor.visit(m_writable);
}

// https://streams.spec.whatwg.org/#transformstream-enqueue
void TransformStream::enqueue(JS::Value chunk)
{
    // To enqueue the JavaScript value chunk into a TransformStream stream, perform ! TransformStreamDefaultControllerEnqueue(stream.[[controller]], chunk).
    MUST(Streams::transform_stream_default_controller_enqueue(*controller(), chunk));
}

// https://streams.spec.whatwg.org/#transformstream-set-up
void TransformStream::set_up(GC::Ref<TransformAlgorithm> transform_algorithm, GC::Ptr<FlushAlgorithm> flush_algorithm, GC::Ptr<CancelAlgorithm> cancel_algorithm)
{
    auto& realm = this->realm();

    // 1. Let writableHighWaterMark be 1.
    auto writable_high_water_mark = 1.0;

    // 2. Let writableSizeAlgorithm be an algorithm that returns 1.
    auto writable_size_algorithm = GC::create_function(realm.heap(), [](JS::Value) {
        return JS::normal_completion(JS::Value { 1 });
    });

    // 3. Let readableHighWaterMark be 0.
    auto readable_high_water_mark = 0.0;

    // 4. Let readableSizeAlgorithm be an algorithm that returns 1.
    auto readable_size_algorithm = GC::create_function(realm.heap(), [](JS::Value) {
        return JS::normal_completion(JS::Value { 1 });
    });

    // 5. Let transformAlgorithmWrapper be an algorithm that runs these steps given a value chunk:
    auto transform_algorithm_wrapper = GC::create_function(realm.heap(), [&realm, transform_algorithm](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        // 1. Let result be the result of running transformAlgorithm given chunk. If this throws an exception e, return a promise rejected with e.
        GC::Ptr<JS::PromiseCapability> result = nullptr;
        result = transform_algorithm->function()(chunk);

        // 2. If result is a Promise, then return result.
        if (result)
            return GC::Ref { *result };

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 6. Let flushAlgorithmWrapper be an algorithm that runs these steps:
    auto flush_algorithm_wrapper = GC::create_function(realm.heap(), [&realm, flush_algorithm]() -> GC::Ref<WebIDL::Promise> {
        // 1. Let result be the result of running flushAlgorithm, if flushAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        GC::Ptr<JS::PromiseCapability> result = nullptr;
        if (flush_algorithm)
            result = flush_algorithm->function()();

        // 2. If result is a Promise, then return result.
        if (result)
            return GC::Ref { *result };

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 7. Let cancelAlgorithmWrapper be an algorithm that runs these steps given a value reason:
    auto cancel_algorithm_wrapper = GC::create_function(realm.heap(), [&realm, cancel_algorithm](JS::Value reason) -> GC::Ref<WebIDL::Promise> {
        // 1. Let result be the result of running cancelAlgorithm given reason, if cancelAlgorithm was given, or null otherwise. If this throws an exception e, return a promise rejected with e.
        GC::Ptr<JS::PromiseCapability> result = nullptr;
        if (cancel_algorithm)
            result = cancel_algorithm->function()(reason);

        // 2. If result is a Promise, then return result.
        if (result)
            return GC::Ref { *result };

        // 3. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 8. Let startPromise be a promise resolved with undefined.
    auto start_promise = WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 9. Perform ! InitializeTransformStream(stream, startPromise, writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark, readableSizeAlgorithm).
    initialize_transform_stream(*this, start_promise, writable_high_water_mark, writable_size_algorithm, readable_high_water_mark, readable_size_algorithm);

    // 10. Let controller be a new TransformStreamDefaultController.
    auto controller = realm.create<TransformStreamDefaultController>(realm);

    // 11. Perform ! SetUpTransformStreamDefaultController(stream, controller, transformAlgorithmWrapper, flushAlgorithmWrapper, cancelAlgorithmWrapper).
    set_up_transform_stream_default_controller(*this, controller, transform_algorithm_wrapper, flush_algorithm_wrapper, cancel_algorithm_wrapper);
}

// https://streams.spec.whatwg.org/#ref-for-transfer-steps②
WebIDL::ExceptionOr<void> TransformStream::transfer_steps(HTML::TransferDataHolder& data_holder)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    auto serialize_stream = [&](auto stream) {
        auto result = MUST(HTML::structured_serialize_with_transfer(vm, stream, { { GC::Root { stream } } }));

        data_holder.data.extend(move(result.transfer_data_holders.first().data));
        data_holder.fds.extend(move(result.transfer_data_holders.first().fds));
    };

    // 1. Let readable be value.[[readable]].
    auto readable = this->readable();

    // 2. Let writable be value.[[writable]].
    auto writable = this->writable();

    // 3. If ! IsReadableStreamLocked(readable) is true, throw a "DataCloneError" DOMException.
    if (is_readable_stream_locked(readable))
        return WebIDL::DataCloneError::create(realm, "Cannot transfer locked ReadableStream"_string);

    // 4. If ! IsWritableStreamLocked(writable) is true, throw a "DataCloneError" DOMException.
    if (is_writable_stream_locked(writable))
        return WebIDL::DataCloneError::create(realm, "Cannot transfer locked WritableStream"_string);

    // 5. Set dataHolder.[[readable]] to ! StructuredSerializeWithTransfer(readable, « readable »).
    serialize_stream(readable);

    // 6. Set dataHolder.[[writable]] to ! StructuredSerializeWithTransfer(writable, « writable »).
    serialize_stream(writable);

    return {};
}

template<typename StreamType>
static WebIDL::ExceptionOr<GC::Ref<StreamType>> deserialize_stream(JS::Realm& realm, HTML::TransferDataHolder& data_holder)
{
    auto transfer_type = data_holder.data.take_first();

    if constexpr (IsSame<StreamType, ReadableStream>)
        VERIFY(transfer_type == to_underlying(HTML::TransferType::ReadableStream));
    else if constexpr (IsSame<StreamType, WritableStream>)
        VERIFY(transfer_type == to_underlying(HTML::TransferType::WritableStream));
    else
        static_assert(DependentFalse<StreamType>);

    auto stream = realm.create<StreamType>(realm);
    TRY(stream->transfer_receiving_steps(data_holder));

    return stream;
}

// https://streams.spec.whatwg.org/#ref-for-transfer-receiving-steps②
WebIDL::ExceptionOr<void> TransformStream::transfer_receiving_steps(HTML::TransferDataHolder& data_holder)
{
    auto& realm = this->realm();

    // 1. Let readableRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[readable]], the current Realm).
    auto readable = TRY(deserialize_stream<ReadableStream>(realm, data_holder));

    // 2. Let writableRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[writable]], the current Realm).
    auto writable = TRY(deserialize_stream<WritableStream>(realm, data_holder));

    // 3. Set value.[[readable]] to readableRecord.[[Deserialized]].
    set_readable(readable);

    // 4. Set value.[[writable]] to writableRecord.[[Deserialized]].
    set_writable(writable);

    // 5. Set value.[[backpressure]], value.[[backpressureChangePromise]], and value.[[controller]] to undefined.
    set_backpressure({});
    set_backpressure_change_promise({});
    set_controller({});

    return {};
}

}
