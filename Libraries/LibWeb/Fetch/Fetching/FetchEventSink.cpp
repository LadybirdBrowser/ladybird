/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibWeb/Fetch/Fetching/FetchEventSink.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Fetch::Fetching {

FetchEventSink::FetchEventSink(Infrastructure::FetchController const& controller)
    : m_controller_token(controller.token())
    , m_controller_registry(controller.registry())
{
}

GC::Ref<Infrastructure::FetchController> FetchEventSink::controller() const
{
    VERIFY(m_controller_registry);
    auto controller = m_controller_registry->find(m_controller_token);
    VERIFY(controller);
    return *controller;
}

// https://fetch.spec.whatwg.org/#concept-http-network-fetch
// The processBodyChunk steps of transmitting request's body.
// NB: The steps read only bytes's length, so they are given it.
static void process_body_chunk(Infrastructure::FetchController& controller, u64 length)
{
    // 1. If fetchParams is canceled, then abort these steps.
    if (controller.control_block().is_canceled())
        return;

    // 2. Run this step in parallel: transmit bytes.
    // NB: The fetch engine loads a body's source whole, with its request.

    // 3. If fetchParams’s process request body chunk length is non-null, then run fetchParams’s process request body
    //    chunk length given bytes’s length.
    if (controller.algorithms()->process_request_body_chunk_length())
        controller.algorithms()->process_request_body_chunk_length()->function()(length);
}

// https://fetch.spec.whatwg.org/#concept-http-network-fetch
// The processEndOfBody steps of transmitting request's body.
static void process_end_of_body(Infrastructure::FetchController& controller)
{
    // 1. If fetchParams is canceled, then abort these steps.
    if (controller.control_block().is_canceled())
        return;

    // 2. If fetchParams’s process request end-of-body is non-null, then run fetchParams’s process request
    //    end-of-body.
    if (controller.algorithms()->process_request_end_of_body())
        controller.algorithms()->process_request_end_of_body()->function()();
}

// https://fetch.spec.whatwg.org/#concept-http-network-fetch
// The step of transmitting request's body for a null body.
static void transmit_null_request_body(GC::Ref<Infrastructure::FetchController> controller)
{
    // 1. If body is null and fetchParams’s process request end-of-body is non-null, then queue a fetch task given
    //    fetchParams’s process request end-of-body and fetchParams’s task destination.
    if (!controller->algorithms()->process_request_end_of_body())
        return;
    Infrastructure::queue_fetch_task(controller, controller->task_destination(), GC::create_function(GC::Heap::the(), GC::captures(controller), [](auto& controller) {
        controller->algorithms()->process_request_end_of_body()->function()();
    }));
}

// https://fetch.spec.whatwg.org/#concept-http-network-fetch
// The steps of transmitting request's body that read a body without a source from its stream.
static void read_request_body_stream(GC::Ref<Infrastructure::FetchController> controller)
{
    auto const& request_body_reader = controller->algorithms()->request_body_reader();
    if (!request_body_reader.has_value())
        return;

    auto const& task_destination = controller->task_destination();
    auto& realm = HTML::relevant_realm(*task_destination.get<GC::Ref<JS::Object>>());

    // 3. Let processBodyError given e be these steps:
    auto process_body_error = GC::create_function(GC::Heap::the(), GC::captures(realm, controller), [](auto& realm, auto& controller, JS::Value error) {
        // 1. If fetchParams is canceled, then abort these steps.
        if (controller->control_block().is_canceled())
            return;

        // 2. If e is an "AbortError" DOMException, then abort fetchParams’s controller.
        auto const* exception = error.is_object() ? Bindings::dom_exception_from_object(error.as_object()) : nullptr;
        if (exception && exception->name() == "AbortError"_utf16_fly_string)
            controller->abort(realm, {});
        // 3. Otherwise, terminate fetchParams’s controller.
        else
            controller->terminate();
    });

    // 4. Incrementally read request’s body given processBodyChunk, processEndOfBody, processBodyError, and
    //    fetchParams’s task destination.
    // FIXME: Send the bytes read here to RequestServer, which only receives a body's source.
    auto process_body_chunk_steps = GC::create_function(GC::Heap::the(), GC::captures(controller), [](auto& controller, ByteBuffer bytes) {
        process_body_chunk(controller, bytes.size());
    });
    auto process_end_of_body_steps = GC::create_function(GC::Heap::the(), GC::captures(controller), [](auto& controller) {
        process_end_of_body(controller);
    });
    HTML::TemporaryExecutionContext const execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    request_body_reader->body->incrementally_read_loop(request_body_reader->reader, task_destination, process_body_chunk_steps, process_end_of_body_steps, process_body_error);
}

// AD-HOC: A body with a source is sent from its source, but its stream has to be read all the same: Canceling a stream
//         teed from it (that of a clone of the Request, e.g.) waits until it is read to its end, or canceled.
static void drain_request_body_stream(GC::Ref<Infrastructure::FetchController> controller)
{
    auto const& request_body_reader = controller->algorithms()->request_body_reader();
    if (!request_body_reader.has_value())
        return;

    auto const& task_destination = controller->task_destination();
    auto& realm = HTML::relevant_realm(*task_destination.get<GC::Ref<JS::Object>>());
    auto ignore_chunk = GC::create_function(GC::Heap::the(), [](ByteBuffer) { });
    auto ignore_end_of_body = GC::create_function(GC::Heap::the(), [] { });
    auto ignore_error = GC::create_function(GC::Heap::the(), [](JS::Value) { });
    HTML::TemporaryExecutionContext const execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    request_body_reader->body->incrementally_read_loop(request_body_reader->reader, task_destination, ignore_chunk, ignore_end_of_body, ignore_error);
}

void FetchEventSink::handle_event(Engine::EngineEvent event)
{
    VERIFY(m_agent_thread_id.is_current_thread());
    auto controller = this->controller();

    event.visit(
        [&](Engine::Event::RequestBodyProgress const& progress) {
            Infrastructure::queue_fetch_task(controller, controller->task_destination(), GC::create_function(GC::Heap::the(), GC::captures(controller, progress.length), [](auto& controller, auto& length) {
                process_body_chunk(controller, length);
            }));
        },
        [&](Engine::Event::RequestBodyEndOfBody const& end_of_body) {
            if (end_of_body.body_is_null) {
                transmit_null_request_body(controller);
                return;
            }
            Infrastructure::queue_fetch_task(controller, controller->task_destination(), GC::create_function(GC::Heap::the(), GC::captures(controller), [](auto& controller) {
                process_end_of_body(controller);
            }));
        },
        [&](Engine::Event::ReadRequestBodyStream const&) {
            read_request_body_stream(controller);
        },
        [&](Engine::Event::RequestBodySentFromSource const&) {
            drain_request_body_stream(controller);
        },
        [&](Engine::Event::NetworkActivity const& network_activity) {
            controller->page()->did_receive_network_activity(network_activity);
        },
        [](Engine::Event::ResponseDelivery const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::EndOfBody const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::ConsumeBodyResult const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::BodyChannelReadable const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::DeliveryEnd const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::RequestTransmissionStarted const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::RequestTransmissionEnded const&) { VERIFY_NOT_REACHED(); });
}

}
