/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/Fetch/Fetching/FetchEventSink.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Fetching/NetworkObserverHooks.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebDriver/BiDi/Network.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Fetch::Fetching {

FetchEventSink::FetchEventSink(Infrastructure::FetchController const& controller)
    : m_controller_token(controller.token())
    , m_controller_registry(controller.registry())
{
}

GC::Ptr<Infrastructure::FetchController> FetchEventSink::controller() const
{
    if (!m_controller_registry)
        return {};
    return m_controller_registry->find(m_controller_token);
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
    controller->queue_fetch_task(GC::create_function(GC::Heap::the(), GC::captures(controller), [](auto& controller) {
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

void report_response_completed_to_observers(Infrastructure::FetchController& controller, Infrastructure::Response const& response, Infrastructure::FetchTimingInfo const& timing_info)
{
    if (!WebDriver::BiDi::network_events_enabled())
        return;
    WebDriver::BiDi::network_response_completed(controller, observed_response(response), timing_info);
}

void FetchEventSink::handle_event(Engine::EngineEvent event)
{
    VERIFY(m_agent_thread_id.is_current_thread());
    if (m_fetch_ended)
        return;
    auto found_controller = this->controller();
    if (!found_controller)
        return;
    GC::Ref controller = *found_controller;

    event.visit(
        [&](Engine::Event::ResponseDelivery& delivery) {
            finish_fetch_response_handover(controller, move(delivery));
        },
        [&](Engine::Event::RequestBodyProgress const& progress) {
            controller->queue_fetch_task(GC::create_function(GC::Heap::the(), GC::captures(controller, progress.length), [](auto& controller, auto& length) {
                process_body_chunk(controller, length);
            }));
        },
        [&](Engine::Event::RequestBodyEndOfBody const& end_of_body) {
            if (end_of_body.body_is_null) {
                transmit_null_request_body(controller);
                return;
            }
            controller->queue_fetch_task(GC::create_function(GC::Heap::the(), GC::captures(controller), [](auto& controller) {
                process_end_of_body(controller);
            }));
        },
        [&](Engine::Event::ReadRequestBodyStream const&) {
            read_request_body_stream(controller);
        },
        [&](Engine::Event::RequestBodySentFromSource const&) {
            drain_request_body_stream(controller);
        },
        [&](Engine::Event::TransportActivity const& activity) {
            controller->page()->did_receive_network_activity(activity);
        },
        [&](Engine::Event::FetchNetworkActivity const& activity) {
            if (!activity.cors_preflight_id.has_value())
                controller->set_observed_request(activity.request);
            WebDriver::BiDi::receive_network_activity(controller, activity);
        },
        [&](Engine::Event::ViolationReport const& report) {
            ContentSecurityPolicy::report_a_violation_of_a_request(controller->realm(), report.violation);
        },
        [&](Engine::Event::RequestTransmissionEnded const& ended) {
            controller->control_block().settle_network_phases(ended.network_phases);
        },
        [&](Engine::Event::EndOfBody const& end_of_body) {
            controller->did_end_body(end_of_body);
        },
        [&](Engine::Event::FetchEnded const&) {
            m_fetch_ended = true;
            controller->did_end_fetch();
        },
        [](Engine::Event::ConsumeBodyResult const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::BodyChannelReadable const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::DeliveryEnd const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Event::RequestTransmissionStarted const&) { VERIFY_NOT_REACHED(); });
}

}
