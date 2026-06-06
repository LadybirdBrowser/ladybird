/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibIPC/File.h>
#include <LibJS/Runtime/Promise.h>
#include <LibWeb/Bindings/ReadableStream.h>
#include <LibWeb/Bindings/TransformStream.h>
#include <LibWeb/Bindings/TransformStreamDefaultController.h>
#include <LibWeb/Bindings/Transformer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Bindings/WritableStream.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>
#include <LibWeb/Streams/TransformStreamOperations.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamOperations.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(TransformStream);

// https://streams.spec.whatwg.org/#ts-constructor
WebIDL::ExceptionOr<GC::Ref<TransformStream>> TransformStream::construct_impl(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, GC::Ptr<JS::Object> transformer_object, Bindings::QueuingStrategy const& writable_strategy, Bindings::QueuingStrategy const& readable_strategy)
{
    auto& realm = HTML::relevant_realm(global_scope);
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto& vm = realm.vm();

    auto stream = GC::Heap::the().allocate<TransformStream>();

    // 1. If transformer is missing, set it to null.
    auto transformer = transformer_object ? JS::Value { transformer_object } : JS::js_null();

    // 2. Let transformerDict be transformer, converted to an IDL value of type Transformer.
    auto transformer_dict = TRY(Bindings::convert_to_idl_value_for_transformer(vm, transformer));

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
    initialize_transform_stream(realm, *stream, start_promise, writable_high_water_mark, writable_size_algorithm, readable_high_water_mark, readable_size_algorithm);
    (void)Bindings::wrap(wrapper_world, realm, stream->readable());
    (void)Bindings::wrap(wrapper_world, realm, stream->writable());

    // 11. Perform ? SetUpTransformStreamDefaultControllerFromTransformer(this, transformer, transformerDict).
    set_up_transform_stream_default_controller_from_transformer(realm, *stream, transformer, transformer_dict);

    // 12. If transformerDict["start"] exists, then resolve startPromise with the result of invoking
    //     transformerDict["start"] with argument list « this.[[controller]] » and callback this value transformer.
    if (transformer_dict.start) {
        auto wrapped_controller = Bindings::wrap(wrapper_world, realm, stream->controller());
        auto result = TRY(WebIDL::invoke_callback(*transformer_dict.start, transformer, { { wrapped_controller } }));
        WebIDL::resolve_promise(realm, start_promise, result);
    }
    // 13. Otherwise, resolve startPromise with undefined.
    else {
        WebIDL::resolve_promise(realm, start_promise, JS::js_undefined());
    }

    return stream;
}

TransformStream::TransformStream()
{
}

TransformStream::~TransformStream() = default;

static ReadableStream* readable_stream_from_object(JS::Object& object)
{
    return Bindings::impl_from<ReadableStream>(&object);
}

static WritableStream* writable_stream_from_object(JS::Object& object)
{
    return Bindings::impl_from<WritableStream>(&object);
}

void TransformStream::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_backpressure_change_promise);
    visitor.visit(m_controller);
    visitor.visit(m_readable);
    visitor.visit(m_writable);
}

JS::Realm& TransformStream::backpressure_change_promise_realm() const
{
    VERIFY(m_backpressure_change_promise);
    return WebIDL::promise_realm(*m_backpressure_change_promise);
}

// https://streams.spec.whatwg.org/#transformstream-enqueue
void TransformStream::enqueue(JS::Value chunk)
{
    // To enqueue the JavaScript value chunk into a TransformStream stream, perform ! TransformStreamDefaultControllerEnqueue(stream.[[controller]], chunk).
    MUST(Streams::transform_stream_default_controller_enqueue(*controller(), chunk));
}

// https://streams.spec.whatwg.org/#transformstream-set-up
void TransformStream::set_up(JS::Realm& realm, GC::Ref<TransformAlgorithm> transform_algorithm, GC::Ptr<FlushAlgorithm> flush_algorithm, GC::Ptr<CancelAlgorithm> cancel_algorithm)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);

    // 1. Let writableHighWaterMark be 1.
    auto writable_high_water_mark = 1.0;

    // 2. Let writableSizeAlgorithm be an algorithm that returns 1.
    auto writable_size_algorithm = GC::create_function(GC::Heap::the(), [](JS::Value) {
        return JS::normal_completion(JS::Value { 1 });
    });

    // 3. Let readableHighWaterMark be 0.
    auto readable_high_water_mark = 0.0;

    // 4. Let readableSizeAlgorithm be an algorithm that returns 1.
    auto readable_size_algorithm = GC::create_function(GC::Heap::the(), [](JS::Value) {
        return JS::normal_completion(JS::Value { 1 });
    });

    // 5. Let transformAlgorithmWrapper be an algorithm that runs these steps given a value chunk:
    auto transform_algorithm_wrapper = GC::create_function(GC::Heap::the(), [&realm, transform_algorithm](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
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
    auto flush_algorithm_wrapper = GC::create_function(GC::Heap::the(), [&realm, flush_algorithm]() -> GC::Ref<WebIDL::Promise> {
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
    auto cancel_algorithm_wrapper = GC::create_function(GC::Heap::the(), [&realm, cancel_algorithm](JS::Value reason) -> GC::Ref<WebIDL::Promise> {
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
    initialize_transform_stream(realm, *this, start_promise, writable_high_water_mark, writable_size_algorithm, readable_high_water_mark, readable_size_algorithm);
    (void)Bindings::wrap(wrapper_world, realm, readable());
    (void)Bindings::wrap(wrapper_world, realm, writable());

    // 10. Let controller be a new TransformStreamDefaultController.
    auto controller = GC::Heap::the().allocate<TransformStreamDefaultController>();

    // 11. Perform ! SetUpTransformStreamDefaultController(stream, controller, transformAlgorithmWrapper, flushAlgorithmWrapper, cancelAlgorithmWrapper).
    set_up_transform_stream_default_controller(*this, controller, transform_algorithm_wrapper, flush_algorithm_wrapper, cancel_algorithm_wrapper);
}

// https://streams.spec.whatwg.org/#ref-for-transfer-steps②
WebIDL::ExceptionOr<void> TransformStream::transfer_steps(JS::Realm& realm, HTML::TransferDataEncoder& data_holder)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);

    // 1. Let readable be value.[[readable]].
    auto readable = this->readable();

    // 2. Let writable be value.[[writable]].
    auto writable = this->writable();

    // 3. If ! IsReadableStreamLocked(readable) is true, throw a "DataCloneError" DOMException.
    if (is_readable_stream_locked(readable))
        return WebIDL::DataCloneError::create(realm, "Cannot transfer locked ReadableStream"_utf16);

    // 4. If ! IsWritableStreamLocked(writable) is true, throw a "DataCloneError" DOMException.
    if (is_writable_stream_locked(writable))
        return WebIDL::DataCloneError::create(realm, "Cannot transfer locked WritableStream"_utf16);

    // 5. Set dataHolder.[[readable]] to ! StructuredSerializeWithTransfer(readable, « readable »).
    auto wrapped_readable = Bindings::wrap(wrapper_world, realm, readable);
    auto readable_result = MUST(HTML::structured_serialize_with_transfer(realm, wrapped_readable, { { wrapped_readable } }));
    data_holder.extend(move(readable_result.transfer_data_holders));

    // 6. Set dataHolder.[[writable]] to ! StructuredSerializeWithTransfer(writable, « writable »).
    auto wrapped_writable = Bindings::wrap(wrapper_world, realm, writable);
    auto writable_result = MUST(HTML::structured_serialize_with_transfer(realm, wrapped_writable, { { wrapped_writable } }));
    data_holder.extend(move(writable_result.transfer_data_holders));

    return {};
}

// https://streams.spec.whatwg.org/#ref-for-transfer-receiving-steps②
WebIDL::ExceptionOr<void> TransformStream::transfer_receiving_steps(JS::Realm& realm, HTML::TransferDataDecoder& data_holder)
{
    // 1. Let readableRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[readable]], the current Realm).
    auto readable_record = MUST(HTML::structured_deserialize_with_transfer_internal(data_holder, realm));

    // 2. Let writableRecord be ! StructuredDeserializeWithTransfer(dataHolder.[[writable]], the current Realm).
    auto writeable_record = MUST(HTML::structured_deserialize_with_transfer_internal(data_holder, realm));

    // 3. Set value.[[readable]] to readableRecord.[[Deserialized]].
    auto* readable = readable_stream_from_object(readable_record.as_object());
    VERIFY(readable);
    set_readable(*readable);

    // 4. Set value.[[writable]] to writableRecord.[[Deserialized]].
    auto* writable = writable_stream_from_object(writeable_record.as_object());
    VERIFY(writable);
    set_writable(*writable);

    // 5. Set value.[[backpressure]], value.[[backpressureChangePromise]], and value.[[controller]] to undefined.
    set_backpressure({});
    set_backpressure_change_promise({});
    set_controller({});

    return {};
}

}
