/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStreamDefaultController.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/Streams/ReadableStreamTee.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams::Detail {

GC_DEFINE_ALLOCATOR(ReadableStreamTeeParams);
GC_DEFINE_ALLOCATOR(ReadableStreamTeeReadRequest);

void ReadableStreamTeeParams::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(reason1);
    visitor.visit(reason2);
    visitor.visit(branch1);
    visitor.visit(branch2);
    visitor.visit(pull_algorithm);
}

// https://streams.spec.whatwg.org/#ref-for-read-request③
ReadableStreamTeeReadRequest::ReadableStreamTeeReadRequest(
    JS::Realm& realm,
    GC::Ref<ReadableStream> stream,
    GC::Ref<ReadableStreamTeeParams> params,
    GC::Ref<WebIDL::Promise> cancel_promise,
    bool clone_for_branch2)
    : m_realm(realm)
    , m_stream(stream)
    , m_params(params)
    , m_cancel_promise(cancel_promise)
    , m_clone_for_branch2(clone_for_branch2)
{
}

void ReadableStreamTeeReadRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_stream);
    visitor.visit(m_params);
    visitor.visit(m_cancel_promise);
}

// https://streams.spec.whatwg.org/#ref-for-read-request-chunk-steps③
void ReadableStreamTeeReadRequest::on_chunk(JS::Value chunk)
{
    // 1. Queue a microtask to perform the following steps:
    HTML::queue_a_microtask(nullptr, GC::create_function(m_realm->heap(), [this, chunk]() -> Coroutine<void> {
        HTML::TemporaryExecutionContext execution_context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto controller1 = m_params->branch1->controller()->get<GC::Ref<ReadableStreamDefaultController>>();
        auto controller2 = m_params->branch2->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

        // 1. Set readAgain to false.
        m_params->read_again = false;

        // 2. Let chunk1 and chunk2 be chunk.
        auto chunk1 = chunk;
        auto chunk2 = chunk;

        // 3. If canceled2 is false and cloneForBranch2 is true,
        if (!m_params->canceled2 && m_clone_for_branch2) {
            // 1. Let cloneResult be StructuredClone(chunk2).
            auto clone_result = structured_clone(m_realm, chunk2);

            // 2. If cloneResult is an abrupt completion,
            if (clone_result.is_exception()) {
                auto completion = Bindings::exception_to_throw_completion(m_realm->vm(), clone_result.release_error());

                // 1. Perform ! ReadableStreamDefaultControllerError(branch1.[[controller]], cloneResult.[[Value]]).
                readable_stream_default_controller_error(controller1, completion.value());

                // 2. Perform ! ReadableStreamDefaultControllerError(branch2.[[controller]], cloneResult.[[Value]]).
                readable_stream_default_controller_error(controller2, completion.value());

                // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                auto cancel_result = readable_stream_cancel(m_stream, completion.value());

                // Note: We need to manually convert the result to an ECMAScript value here, by extracting its [[Promise]] slot.
                WebIDL::resolve_promise(m_realm, m_cancel_promise, cancel_result->promise());

                // 4. Return.
                co_return;
            }

            // 3. Otherwise, set chunk2 to cloneResult.[[Value]].
            chunk2 = clone_result.release_value();
        }

        // 4. If canceled1 is false, perform ! ReadableStreamDefaultControllerEnqueue(branch1.[[controller]], chunk1).
        if (!m_params->canceled1) {
            MUST(readable_stream_default_controller_enqueue(controller1, chunk1));
        }

        // 5. If canceled2 is false, perform ! ReadableStreamDefaultControllerEnqueue(branch2.[[controller]], chunk2).
        if (!m_params->canceled2) {
            MUST(readable_stream_default_controller_enqueue(controller2, chunk2));
        }

        // 6. Set reading to false.
        m_params->reading = false;

        // 7. If readAgain is true, perform pullAlgorithm.
        if (m_params->read_again) {
            (void)m_params->pull_algorithm->function()();
        }
    }));

    // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
    //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
    //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
}

// https://streams.spec.whatwg.org/#ref-for-read-request-close-steps②
void ReadableStreamTeeReadRequest::on_close()
{
    auto controller1 = m_params->branch1->controller()->get<GC::Ref<ReadableStreamDefaultController>>();
    auto controller2 = m_params->branch2->controller()->get<GC::Ref<ReadableStreamDefaultController>>();

    // 1. Set reading to false.
    m_params->reading = false;

    // 2. If canceled1 is false, perform ! ReadableStreamDefaultControllerClose(branch1.[[controller]]).
    if (!m_params->canceled1) {
        readable_stream_default_controller_close(controller1);
    }

    // 3. If canceled2 is false, perform ! ReadableStreamDefaultControllerClose(branch2.[[controller]]).
    if (!m_params->canceled2) {
        readable_stream_default_controller_close(controller2);
    }

    // 4. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
    if (!m_params->canceled1 || !m_params->canceled2) {
        WebIDL::resolve_promise(m_realm, m_cancel_promise, JS::js_undefined());
    }
}

// https://streams.spec.whatwg.org/#ref-for-read-request-error-steps③
void ReadableStreamTeeReadRequest::on_error(JS::Value)
{
    // 1. Set reading to false.
    m_params->reading = false;
}

GC_DEFINE_ALLOCATOR(ReadableByteStreamTeeParams);
GC_DEFINE_ALLOCATOR(ReadableByteStreamTeeDefaultReadRequest);
GC_DEFINE_ALLOCATOR(ReadableByteStreamTeeBYOBReadRequest);

ReadableByteStreamTeeParams::ReadableByteStreamTeeParams(ReadableStreamReader reader)
    : reader(move(reader))
{
}

void ReadableByteStreamTeeParams::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(reason1);
    visitor.visit(reason2);
    visitor.visit(branch1);
    visitor.visit(branch2);
    visitor.visit(pull1_algorithm);
    visitor.visit(pull2_algorithm);
    reader.visit([&](auto underlying_reader) { visitor.visit(underlying_reader); });
}

// https://streams.spec.whatwg.org/#ref-for-read-request④
ReadableByteStreamTeeDefaultReadRequest::ReadableByteStreamTeeDefaultReadRequest(
    JS::Realm& realm,
    GC::Ref<ReadableStream> stream,
    GC::Ref<ReadableByteStreamTeeParams> params,
    GC::Ref<WebIDL::Promise> cancel_promise)
    : m_realm(realm)
    , m_stream(stream)
    , m_params(params)
    , m_cancel_promise(cancel_promise)
{
}

void ReadableByteStreamTeeDefaultReadRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_stream);
    visitor.visit(m_params);
    visitor.visit(m_cancel_promise);
}

// https://streams.spec.whatwg.org/#ref-for-read-request-chunk-steps④
void ReadableByteStreamTeeDefaultReadRequest::on_chunk(JS::Value chunk)
{
    // 1. Queue a microtask to perform the following steps:
    HTML::queue_a_microtask(nullptr, GC::create_function(m_realm->heap(), [this, chunk]() mutable -> Coroutine<void> {
        HTML::TemporaryExecutionContext execution_context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto controller1 = m_params->branch1->controller()->get<GC::Ref<ReadableByteStreamController>>();
        auto controller2 = m_params->branch2->controller()->get<GC::Ref<ReadableByteStreamController>>();

        // 1. Set readAgainForBranch1 to false.
        m_params->read_again_for_branch1 = false;

        // 2. Set readAgainForBranch2 to false.
        m_params->read_again_for_branch2 = false;

        // 3. Let chunk1 and chunk2 be chunk.
        auto chunk1 = chunk;
        auto chunk2 = chunk;

        // 4. If canceled1 is false and canceled2 is false,
        if (!m_params->canceled1 && !m_params->canceled2) {
            // 1. Let cloneResult be CloneAsUint8Array(chunk).
            auto chunk_view = m_realm->create<WebIDL::ArrayBufferView>(chunk.as_object());
            auto clone_result = clone_as_uint8_array(m_realm, chunk_view);

            // 2. If cloneResult is an abrupt completion,
            if (clone_result.is_exception()) {
                auto completion = Bindings::exception_to_throw_completion(m_realm->vm(), clone_result.release_error());

                // 1. Perform ! ReadableByteStreamControllerError(branch1.[[controller]], cloneResult.[[Value]]).
                readable_byte_stream_controller_error(controller1, completion.value());

                // 2. Perform ! ReadableByteStreamControllerError(branch2.[[controller]], cloneResult.[[Value]]).
                readable_byte_stream_controller_error(controller2, completion.value());

                // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                auto cancel_result = readable_stream_cancel(m_stream, completion.value());

                WebIDL::resolve_promise(m_realm, m_cancel_promise, cancel_result->promise());

                // 4. Return.
                co_return;
            }

            // 3. Otherwise, set chunk2 to cloneResult.[[Value]].
            chunk2 = clone_result.release_value();
        }

        // 5. If canceled1 is false, perform ! ReadableByteStreamControllerEnqueue(branch1.[[controller]], chunk1).
        if (!m_params->canceled1) {
            MUST(readable_byte_stream_controller_enqueue(controller1, chunk1));
        }

        // 6. If canceled2 is false, perform ! ReadableByteStreamControllerEnqueue(branch2.[[controller]], chunk2).
        if (!m_params->canceled2) {
            MUST(readable_byte_stream_controller_enqueue(controller2, chunk2));
        }

        // 7. Set reading to false.
        m_params->reading = false;

        // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
        if (m_params->read_again_for_branch1) {
            (void)m_params->pull1_algorithm->function()();
        }
        // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
        else if (m_params->read_again_for_branch2) {
            (void)m_params->pull2_algorithm->function()();
        }
    }));

    // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
    //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
    //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
}

// https://streams.spec.whatwg.org/#ref-for-read-request-close-steps③
void ReadableByteStreamTeeDefaultReadRequest::on_close()
{
    auto controller1 = m_params->branch1->controller()->get<GC::Ref<ReadableByteStreamController>>();
    auto controller2 = m_params->branch2->controller()->get<GC::Ref<ReadableByteStreamController>>();

    // 1. Set reading to false.
    m_params->reading = false;

    // 2. If canceled1 is false, perform ! ReadableByteStreamControllerClose(branch1.[[controller]]).
    if (!m_params->canceled1) {
        MUST(readable_byte_stream_controller_close(controller1));
    }

    // 3. If canceled2 is false, perform ! ReadableByteStreamControllerClose(branch2.[[controller]]).
    if (!m_params->canceled2) {
        MUST(readable_byte_stream_controller_close(controller2));
    }

    // 4. If branch1.[[controller]].[[pendingPullIntos]] is not empty, perform ! ReadableByteStreamControllerRespond(branch1.[[controller]], 0).
    if (!controller1->pending_pull_intos().is_empty()) {
        MUST(readable_byte_stream_controller_respond(controller1, 0));
    }

    // 5. If branch2.[[controller]].[[pendingPullIntos]] is not empty, perform ! ReadableByteStreamControllerRespond(branch2.[[controller]], 0).
    if (!controller2->pending_pull_intos().is_empty()) {
        MUST(readable_byte_stream_controller_respond(controller2, 0));
    }

    // 6. If canceled1 is false or canceled2 is false, resolve cancelPromise with undefined.
    if (!m_params->canceled1 || !m_params->canceled2) {
        WebIDL::resolve_promise(m_realm, m_cancel_promise, JS::js_undefined());
    }
}

// https://streams.spec.whatwg.org/#ref-for-read-request-error-steps④
void ReadableByteStreamTeeDefaultReadRequest::on_error(JS::Value)
{
    // 1. Set reading to false.
    m_params->reading = false;
}

// https://streams.spec.whatwg.org/#ref-for-read-into-request②
ReadableByteStreamTeeBYOBReadRequest::ReadableByteStreamTeeBYOBReadRequest(
    JS::Realm& realm,
    GC::Ref<ReadableStream> stream,
    GC::Ref<ReadableByteStreamTeeParams> params,
    GC::Ref<WebIDL::Promise> cancel_promise,
    GC::Ref<ReadableStream> byob_branch,
    GC::Ref<ReadableStream> other_branch,
    bool for_branch2)
    : m_realm(realm)
    , m_stream(stream)
    , m_params(params)
    , m_cancel_promise(cancel_promise)
    , m_byob_branch(byob_branch)
    , m_other_branch(other_branch)
    , m_for_branch2(for_branch2)
{
}

void ReadableByteStreamTeeBYOBReadRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_stream);
    visitor.visit(m_params);
    visitor.visit(m_cancel_promise);
    visitor.visit(m_byob_branch);
    visitor.visit(m_other_branch);
}

// https://streams.spec.whatwg.org/#ref-for-read-into-request-chunk-steps①
void ReadableByteStreamTeeBYOBReadRequest::on_chunk(JS::Value chunk)
{
    auto chunk_view = m_realm->create<WebIDL::ArrayBufferView>(chunk.as_object());

    // 1. Queue a microtask to perform the following steps:
    HTML::queue_a_microtask(nullptr, GC::create_function(m_realm->heap(), [this, chunk = chunk_view]() -> Coroutine<void> {
        HTML::TemporaryExecutionContext execution_context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto byob_controller = m_byob_branch->controller()->get<GC::Ref<ReadableByteStreamController>>();
        auto other_controller = m_other_branch->controller()->get<GC::Ref<ReadableByteStreamController>>();

        // 1. Set readAgainForBranch1 to false.
        m_params->read_again_for_branch1 = false;

        // 2. Set readAgainForBranch2 to false.
        m_params->read_again_for_branch2 = false;

        // 3. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1 otherwise.
        auto byob_cancelled = m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

        // 4. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1 otherwise.
        auto other_cancelled = !m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

        // 5. If otherCanceled is false,
        if (!other_cancelled) {
            // 1. Let cloneResult be CloneAsUint8Array(chunk).
            auto clone_result = clone_as_uint8_array(m_realm, chunk);

            // 2. If cloneResult is an abrupt completion,
            if (clone_result.is_exception()) {
                auto completion = Bindings::exception_to_throw_completion(m_realm->vm(), clone_result.release_error());

                // 1. Perform ! ReadableByteStreamControllerError(byobBranch.[[controller]], cloneResult.[[Value]]).
                readable_byte_stream_controller_error(byob_controller, completion.value());

                // 2. Perform ! ReadableByteStreamControllerError(otherBranch.[[controller]], cloneResult.[[Value]]).
                readable_byte_stream_controller_error(other_controller, completion.value());

                // 3. Resolve cancelPromise with ! ReadableStreamCancel(stream, cloneResult.[[Value]]).
                auto cancel_result = readable_stream_cancel(m_stream, completion.value());

                WebIDL::resolve_promise(m_realm, m_cancel_promise, cancel_result->promise());

                // 4. Return.
                co_return;
            }

            // 3. Otherwise, let clonedChunk be cloneResult.[[Value]].
            auto cloned_chunk = clone_result.release_value();

            // 4. If byobCanceled is false, perform ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
            if (!byob_cancelled) {
                MUST(readable_byte_stream_controller_respond_with_new_view(m_realm, byob_controller, chunk));
            }

            // 5. Perform ! ReadableByteStreamControllerEnqueue(otherBranch.[[controller]], clonedChunk).
            MUST(readable_byte_stream_controller_enqueue(other_controller, cloned_chunk));
        }
        // 6. Otherwise, if byobCanceled is false, perform ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
        else if (!byob_cancelled) {
            MUST(readable_byte_stream_controller_respond_with_new_view(m_realm, byob_controller, chunk));
        }

        // 7. Set reading to false.
        m_params->reading = false;

        // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
        if (m_params->read_again_for_branch1) {
            (void)m_params->pull1_algorithm->function()();
        }
        // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
        else if (m_params->read_again_for_branch2) {
            (void)m_params->pull2_algorithm->function()();
        }
    }));

    // NOTE: The microtask delay here is necessary because it takes at least a microtask to detect errors, when we
    //       use reader.[[closedPromise]] below. We want errors in stream to error both branches immediately, so we
    //       cannot let successful synchronously-available reads happen ahead of asynchronously-available errors.
}

// https://streams.spec.whatwg.org/#ref-for-read-into-request-close-steps②
void ReadableByteStreamTeeBYOBReadRequest::on_close(JS::Value chunk)
{
    auto byob_controller = m_byob_branch->controller()->get<GC::Ref<ReadableByteStreamController>>();
    auto other_controller = m_other_branch->controller()->get<GC::Ref<ReadableByteStreamController>>();

    // 1. Set reading to false.
    m_params->reading = false;

    // 2. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1 otherwise.
    auto byob_cancelled = m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

    // 3. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1 otherwise.
    auto other_cancelled = !m_for_branch2 ? m_params->canceled2 : m_params->canceled1;

    // 4. If byobCanceled is false, perform ! ReadableByteStreamControllerClose(byobBranch.[[controller]]).
    if (!byob_cancelled) {
        MUST(readable_byte_stream_controller_close(byob_controller));
    }

    // 5. If otherCanceled is false, perform ! ReadableByteStreamControllerClose(otherBranch.[[controller]]).
    if (!other_cancelled) {
        MUST(readable_byte_stream_controller_close(other_controller));
    }

    // 6. If chunk is not undefined,
    if (!chunk.is_undefined()) {
        // 1. Assert: chunk.[[ByteLength]] is 0.

        // 2. If byobCanceled is false, perform ! ReadableByteStreamControllerRespondWithNewView(byobBranch.[[controller]], chunk).
        if (!byob_cancelled) {
            auto array_buffer_view = m_realm->create<WebIDL::ArrayBufferView>(chunk.as_object());
            MUST(readable_byte_stream_controller_respond_with_new_view(m_realm, byob_controller, array_buffer_view));
        }

        // 3. If otherCanceled is false and otherBranch.[[controller]].[[pendingPullIntos]] is not empty,
        //    perform ! ReadableByteStreamControllerRespond(otherBranch.[[controller]], 0).
        if (!other_cancelled && !other_controller->pending_pull_intos().is_empty()) {
            MUST(readable_byte_stream_controller_respond(other_controller, 0));
        }
    }

    // 7. If byobCanceled is false or otherCanceled is false, resolve cancelPromise with undefined.
    if (!byob_cancelled || !other_cancelled) {
        WebIDL::resolve_promise(m_realm, m_cancel_promise, JS::js_undefined());
    }
}

// https://streams.spec.whatwg.org/#ref-for-read-into-request-error-steps①
void ReadableByteStreamTeeBYOBReadRequest::on_error(JS::Value)
{
    // 1. Set reading to false.
    m_params->reading = false;
}

}
