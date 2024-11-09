/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TransformStreamPrototype.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>
#include <LibWeb/Streams/Transformer.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

JS_DEFINE_ALLOCATOR(TransformStream);

// https://streams.spec.whatwg.org/#ts-constructor
WebIDL::ExceptionOr<JS::NonnullGCPtr<TransformStream>> TransformStream::construct_impl(JS::Realm& realm, Optional<JS::Handle<JS::Object>> transformer_object, QueuingStrategy const& writable_strategy, QueuingStrategy const& readable_strategy)
{
    auto& vm = realm.vm();

    auto stream = realm.heap().allocate<TransformStream>(realm, realm);

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
    initialize_transform_stream(*stream, start_promise, writable_high_water_mark, move(writable_size_algorithm), readable_high_water_mark, move(readable_size_algorithm));

    // 11. Perform ? SetUpTransformStreamDefaultControllerFromTransformer(this, transformer, transformerDict).
    set_up_transform_stream_default_controller_from_transformer(*stream, transformer, transformer_dict);

    // 12. If transformerDict["start"] exists, then resolve startPromise with the result of invoking
    //     transformerDict["start"] with argument list « this.[[controller]] » and callback this value transformer.
    if (transformer_dict.start) {
        auto result = TRY(WebIDL::invoke_callback(*transformer_dict.start, transformer, stream->controller())).release_value();
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TransformStream);
}

void TransformStream::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_backpressure_change_promise);
    visitor.visit(m_controller);
    visitor.visit(m_readable);
    visitor.visit(m_writable);
}

}
