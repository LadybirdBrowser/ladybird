/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WritableStreamDefaultControllerPrototype.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/Streams/WritableStreamDefaultController.h>

namespace Web::Streams {

JS_DEFINE_ALLOCATOR(WritableStreamDefaultController);

void WritableStreamDefaultController::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_signal);
    for (auto& value : m_queue)
        visitor.visit(value.value);
    visitor.visit(m_stream);
    visitor.visit(m_abort_algorithm);
    visitor.visit(m_close_algorithm);
    visitor.visit(m_strategy_size_algorithm);
    visitor.visit(m_write_algorithm);
}

// https://streams.spec.whatwg.org/#ws-default-controller-error
void WritableStreamDefaultController::error(JS::Value error)
{
    // 1. Let state be this.[[stream]].[[state]].
    auto state = m_stream->state();

    // 2. If state is not "writable", return.
    if (state != WritableStream::State::Writable)
        return;

    // 3. Perform ! WritableStreamDefaultControllerError(this, e).
    writable_stream_default_controller_error(*this, error);
}

// https://streams.spec.whatwg.org/#ws-default-controller-private-abort
JS::NonnullGCPtr<WebIDL::Promise> WritableStreamDefaultController::abort_steps(JS::Value reason)
{
    // 1. Let result be the result of performing this.[[abortAlgorithm]], passing reason.
    auto result = m_abort_algorithm->function()(reason);

    // 2. Perform ! WritableStreamDefaultControllerClearAlgorithms(this).
    writable_stream_default_controller_clear_algorithms(*this);

    // 3. Return result.
    return result;
}

// https://streams.spec.whatwg.org/#ws-default-controller-private-error
void WritableStreamDefaultController::error_steps()
{
    // 1. Perform ! ResetQueue(this).
    reset_queue(*this);
}

WritableStreamDefaultController::WritableStreamDefaultController(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

}
