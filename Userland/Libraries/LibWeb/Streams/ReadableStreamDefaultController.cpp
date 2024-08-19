/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ReadableStreamDefaultControllerPrototype.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ReadableStreamDefaultController);

ReadableStreamDefaultController::ReadableStreamDefaultController(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

// https://streams.spec.whatwg.org/#rs-default-controller-desired-size
Optional<double> ReadableStreamDefaultController::desired_size()
{
    // 1. Return ! ReadableStreamDefaultControllerGetDesiredSize(this).
    return readable_stream_default_controller_get_desired_size(*this);
}

// https://streams.spec.whatwg.org/#rs-default-controller-close
WebIDL::ExceptionOr<void> ReadableStreamDefaultController::close()
{
    // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is false, throw a TypeError exception.
    if (!readable_stream_default_controller_can_close_or_enqueue(*this)) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Stream is not closable"sv };
    }

    // 2. Perform ! ReadableStreamDefaultControllerClose(this).
    readable_stream_default_controller_close(*this);

    return {};
}

// https://streams.spec.whatwg.org/#rs-default-controller-enqueue
WebIDL::ExceptionOr<void> ReadableStreamDefaultController::enqueue(JS::Value chunk)
{
    // 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is false, throw a TypeError exception.
    if (!readable_stream_default_controller_can_close_or_enqueue(*this))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot enqueue chunk to stream"sv };

    // 2. Perform ? ReadableStreamDefaultControllerEnqueue(this, chunk).
    TRY(readable_stream_default_controller_enqueue(*this, chunk));

    return {};
}

// https://streams.spec.whatwg.org/#rs-default-controller-error
void ReadableStreamDefaultController::error(JS::Value error)
{
    // 1. Perform ! ReadableStreamDefaultControllerError(this, e).
    readable_stream_default_controller_error(*this, error);
}

// https://streams.spec.whatwg.org/#rs-default-controller-private-cancel
GC::Ref<WebIDL::Promise> ReadableStreamDefaultController::cancel_steps(JS::Value reason)
{
    // 1. Perform ! ResetQueue(this).
    reset_queue(*this);

    // 2. Let result be the result of performing this.[[cancelAlgorithm]], passing reason.
    auto result = cancel_algorithm()->function()(reason);

    // 3. Perform ! ReadableStreamDefaultControllerClearAlgorithms(this).
    readable_stream_default_controller_clear_algorithms(*this);

    // 4. Return result.
    return result;
}

// https://streams.spec.whatwg.org/#rs-default-controller-private-pull
void ReadableStreamDefaultController::pull_steps(Web::Streams::ReadRequest& read_request)
{
    // 1. Let stream be this.[[stream]].
    auto& stream = *m_stream;

    // 2. If this.[[queue]] is not empty,
    if (!m_queue.is_empty()) {
        // 1. Let chunk be ! DequeueValue(this).
        auto chunk = dequeue_value(*this);

        // 2. If this.[[closeRequested]] is true and this.[[queue]] is empty,
        if (m_close_requested && m_queue.is_empty()) {
            // 1. Perform ! ReadableStreamDefaultControllerClearAlgorithms(this).
            readable_stream_default_controller_clear_algorithms(*this);

            // 2. Perform ! ReadableStreamClose(stream).
            readable_stream_close(stream);
        }
        // 3. Otherwise, perform ! ReadableStreamDefaultControllerCallPullIfNeeded(this).
        else {
            readable_stream_default_controller_can_pull_if_needed(*this);
        }

        // 4. Perform readRequest’s chunk steps, given chunk.
        read_request.on_chunk(chunk);
    }
    // 3. Otherwise,
    else {
        // 1. Perform ! ReadableStreamAddReadRequest(stream, readRequest).
        readable_stream_add_read_request(stream, read_request);

        // 2. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(this).
        readable_stream_default_controller_can_pull_if_needed(*this);
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultcontroller-releasesteps
void ReadableStreamDefaultController::release_steps()
{
    // 1. Return.
}

void ReadableStreamDefaultController::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ReadableStreamDefaultController);
}

void ReadableStreamDefaultController::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& item : m_queue)
        visitor.visit(item.value);
    visitor.visit(m_stream);
    visitor.visit(m_cancel_algorithm);
    visitor.visit(m_pull_algorithm);
    visitor.visit(m_strategy_size_algorithm);
}

}
