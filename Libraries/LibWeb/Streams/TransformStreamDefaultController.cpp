/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TransformStreamDefaultControllerPrototype.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>

namespace Web::Streams {

JS_DEFINE_ALLOCATOR(TransformStreamDefaultController);

TransformStreamDefaultController::TransformStreamDefaultController(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

TransformStreamDefaultController::~TransformStreamDefaultController() = default;

void TransformStreamDefaultController::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TransformStreamDefaultController);
}

void TransformStreamDefaultController::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stream);
    visitor.visit(m_cancel_algorithm);
    visitor.visit(m_finish_promise);
    visitor.visit(m_flush_algorithm);
    visitor.visit(m_transform_algorithm);
}

// https://streams.spec.whatwg.org/#ts-default-controller-desired-size
Optional<double> TransformStreamDefaultController::desired_size()
{
    VERIFY(stream()->readable()->controller().has_value() && stream()->readable()->controller()->has<JS::NonnullGCPtr<ReadableStreamDefaultController>>());

    // 1. Let readableController be this.[[stream]].[[readable]].[[controller]].
    auto readable_controller = stream()->readable()->controller()->get<JS::NonnullGCPtr<ReadableStreamDefaultController>>();

    // 2. Return ! ReadableStreamDefaultControllerGetDesiredSize(readableController).
    return readable_stream_default_controller_get_desired_size(*readable_controller);
}

// https://streams.spec.whatwg.org/#ts-default-controller-enqueue
WebIDL::ExceptionOr<void> TransformStreamDefaultController::enqueue(Optional<JS::Value> chunk)
{
    // 1. Perform ? TransformStreamDefaultControllerEnqueue(this, chunk).
    TRY(transform_stream_default_controller_enqueue(*this, chunk.has_value() ? chunk.value() : JS::js_undefined()));

    return {};
}

// https://streams.spec.whatwg.org/#ts-default-controller-error
void TransformStreamDefaultController::error(Optional<JS::Value> reason)
{
    // 1. Perform ? TransformStreamDefaultControllerError(this, e).
    transform_stream_default_controller_error(*this, reason.has_value() ? reason.value() : JS::js_undefined());
}

// https://streams.spec.whatwg.org/#ts-default-controller-terminate
void TransformStreamDefaultController::terminate()
{
    // 1. Perform ? TransformStreamDefaultControllerTerminate(this).
    transform_stream_default_controller_terminate(*this);
}

}
