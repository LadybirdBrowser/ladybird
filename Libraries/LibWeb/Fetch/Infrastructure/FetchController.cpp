/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/Network.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(FetchController);

FetchController::FetchController(FetchControllerRegistry& registry)
    : m_registry(registry)
    , m_token(registry.add({}, *this))
    , m_control_block(FetchControlBlock::create())
{
}

GC::Ref<FetchController> FetchController::create(FetchControllerRegistry& registry)
{
    return GC::Heap::the().allocate<FetchController>(registry);
}

void FetchController::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_report_timing_steps);
    if (m_awaited_network_completion.has_value())
        visitor.visit(m_awaited_network_completion->steps);
    visitor.visit(m_next_manual_redirect_steps);
    visitor.visit(m_algorithms);
    visitor.visit(m_task_destination);
    visitor.visit(m_pending_preloaded_response);
    visitor.visit(m_stop_steps);
    visitor.visit(m_pending_responses);
}

void FetchController::finalize()
{
    // A discarded response can leave an ongoing controller without a stop or end-of-body task.
    end_rendering_pending_operation(this);
    Base::finalize();
    if (m_registry)
        m_registry->remove({}, m_token);
}

void FetchController::set_algorithms(Badge<FetchParams>, GC::Ref<FetchAlgorithms const> algorithms)
{
    VERIFY(!m_algorithms);
    m_algorithms = algorithms;
    m_control_block->set_body_intent({}, algorithms->body_intent());
}

void FetchController::retarget(TaskDestination task_destination, Engine::BodyIntent body_intent)
{
    m_task_destination = move(task_destination);
    m_control_block->set_body_intent({}, body_intent);
}

void FetchController::set_original_fetch_params(Badge<FetchParams>, FetchParams const& fetch_params)
{
    m_original_fetch_params = fetch_params;
}

void FetchController::report_webdriver_bidi_fetch_error(StringView error_text) const
{
    if (!WebDriver::BiDi::g_network_events_enabled)
        return;
    if (auto fetch_params = m_original_fetch_params.strong_ref())
        WebDriver::BiDi::network_fetch_error(*fetch_params, error_text);
}

void FetchController::cancel_webdriver_bidi_network_interception() const
{
    if (!WebDriver::BiDi::is_active())
        return;
    if (auto fetch_params = original_fetch_params())
        WebDriver::BiDi::cancel_network_interception(*fetch_params);
}

RefPtr<FetchParams> FetchController::original_fetch_params() const
{
    return m_original_fetch_params.strong_ref();
}

GC::Ref<FetchAlgorithms const> FetchController::algorithms() const
{
    VERIFY(m_algorithms);
    return *m_algorithms;
}

void FetchController::set_network_transport(Engine::NetworkTransportHandle& network_transport)
{
    set_body_transport(network_transport);
    m_control_block->set_has_started_request({});
}

void FetchController::set_body_transport(Engine::FetchBodyDeliveryGate::Transport& body_transport)
{
    m_body_transport = body_transport;
}

void FetchController::set_report_timing_steps(Function<void(JS::Object&)> report_timing_steps)
{
    m_report_timing_steps = GC::create_function(GC::Heap::the(), move(report_timing_steps));
}

void FetchController::set_next_manual_redirect_steps(Function<void()> next_manual_redirect_steps)
{
    m_next_manual_redirect_steps = GC::create_function(GC::Heap::the(), move(next_manual_redirect_steps));
}

// https://fetch.spec.whatwg.org/#finalize-and-report-timing
void FetchController::report_timing(JS::Object& global) const
{
    // 1. Assert: this’s report timing steps is not null.
    VERIFY(m_report_timing_steps);

    // 2. Call this’s report timing steps with global.
    m_report_timing_steps->function()(global);
}

// https://fetch.spec.whatwg.org/#fetch-controller-process-the-next-manual-redirect
void FetchController::process_next_manual_redirect() const
{
    // 1. Assert: controller’s next manual redirect steps are not null.
    VERIFY(m_next_manual_redirect_steps);

    // 2. Call controller’s next manual redirect steps.
    m_next_manual_redirect_steps->function()();
}

void FetchController::when_network_completes_body(Engine::FetchByteChannel const& body, GC::Ref<GC::Function<void()>> steps)
{
    m_awaited_network_completion = AwaitedNetworkCompletion { body, steps };
}

void FetchController::did_complete_network_body(Engine::FetchByteChannel const& body)
{
    if (!m_awaited_network_completion.has_value() || m_awaited_network_completion->body.ptr() != &body)
        return;
    auto steps = m_awaited_network_completion.release_value().steps;
    steps->function()();
}

// https://fetch.spec.whatwg.org/#extract-full-timing-info
NonnullRefPtr<FetchTimingInfo> FetchController::extract_full_timing_info() const
{
    // 1. Assert: this’s full timing info is not null.
    auto full_timing_info = m_control_block->full_timing_info();
    VERIFY(full_timing_info);

    // 2. Return this’s full timing info.
    return full_timing_info.release_nonnull();
}

// https://fetch.spec.whatwg.org/#fetch-controller-abort
void FetchController::abort(JS::Realm& realm, Optional<JS::Value> error)
{
    // 1. Set controller’s state to "aborted".
    m_control_block->set_state({}, State::Aborted);
    report_webdriver_bidi_fetch_error("Fetch was aborted"sv);
    cancel_webdriver_bidi_network_interception();

    // 2. Let fallbackError be an "AbortError" DOMException.
    auto fallback_error = WebIDL::AbortError::create("Fetch was aborted"_utf16);
    auto fallback_error_value = throw_completion(realm, fallback_error).value();

    // 3. Set error to fallbackError if it is not given.
    if (!error.has_value())
        error = fallback_error_value;

    // 4. Let serializedError be StructuredSerialize(error). If that threw an exception, catch it, and let serializedError be StructuredSerialize(fallbackError).
    // 5. Set controller’s serialized abort reason to serializedError
    auto structured_serialize = [&realm](JS::Value error, JS::Value fallback_error) {
        auto serialized_value_or_error = HTML::structured_serialize(realm.vm(), error);
        return serialized_value_or_error.is_error()
            ? HTML::structured_serialize(realm.vm(), fallback_error).value()
            : serialized_value_or_error.value();
    };
    m_serialized_abort_reason = structured_serialize(error.value(), fallback_error_value);

    stop_request();
}

// https://fetch.spec.whatwg.org/#fetch-controller-terminate
void FetchController::terminate()
{
    // To terminate a fetch controller controller, set controller’s state to "terminated".
    m_control_block->set_state({}, State::Terminated);
    report_webdriver_bidi_fetch_error("Fetch was terminated"sv);
    cancel_webdriver_bidi_network_interception();

    stop_request();
}

void FetchController::stop_fetch()
{
    if (state() == State::Aborted || state() == State::Terminated)
        return;

    m_control_block->set_state({}, State::Stopped);
    report_webdriver_bidi_fetch_error("Fetch was stopped"sv);
    cancel_webdriver_bidi_network_interception();

    // AD-HOC: Some HTML elements need to stop an ongoing fetching process without causing any network error to be raised
    //         (which abort() and terminate() will both do). This is tricky because the fetch process runs across several
    //         nested Platform::EventLoopPlugin::deferred_invoke() invocations. For now, we "stop" the fetch process by
    //         cancelling any queued fetch tasks and then ignoring any callbacks.
    auto ongoing_fetch_tasks = move(m_ongoing_fetch_tasks);

    HTML::main_thread_event_loop().task_queue().remove_tasks_matching([&](auto const& task) {
        return ongoing_fetch_tasks.remove_all_matching([&](u64, HTML::TaskID task_id) {
            return task.id() == task_id;
        });
    });

    if (m_algorithms)
        m_algorithms = FetchAlgorithms::create(FetchAlgorithms::Input { m_control_block->body_intent() });

    stop_request();

    if (auto stop_steps = m_stop_steps) {
        m_stop_steps = nullptr;
        stop_steps->function()();
    }
}

// Stops the network request of a fetch that is no longer ongoing. HTTP-network fetch reads a response's body in steps
// that "abort when fetchParams is canceled" (step 18.1) — and once aborted, "the user agent should close connection
// unless it would be bad for performance to do so" (step 18.2.4). So a canceled fetch's request has to stop as well as
// a stopped one's: otherwise it streams on until the server ends the response, and its callbacks root the fetch's realm
// for all that time.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
void FetchController::stop_request()
{
    end_rendering_pending_operation(this);
    VERIFY(state() != State::Ongoing);
    m_awaited_network_completion.clear();
    stop_body_transport();
}

void FetchController::stop_body_transport()
{
    if (auto body_transport = m_body_transport.strong_ref()) {
        m_body_transport = nullptr;
        body_transport->stop();
    }
}

void FetchController::fetch_task_queued(u64 fetch_task_id, HTML::TaskID event_id)
{
    m_ongoing_fetch_tasks.set(fetch_task_id, event_id);
}

void FetchController::fetch_task_complete(u64 fetch_task_id)
{
    m_ongoing_fetch_tasks.remove(fetch_task_id);
}

void FetchController::add_pending_response(Badge<Fetching::PendingResponse>, GC::Ref<Fetching::PendingResponse> pending_response)
{
    VERIFY(!m_pending_responses.contains_slow(pending_response));
    m_pending_responses.append(pending_response);
}

void FetchController::remove_pending_response(Badge<Fetching::PendingResponse>, GC::Ref<Fetching::PendingResponse> pending_response)
{
    m_pending_responses.remove_first_matching([&](auto entry) { return entry == pending_response; });
}

GC_DEFINE_ALLOCATOR(FetchControllerHolder);

FetchControllerHolder::FetchControllerHolder() = default;

GC::Ref<FetchControllerHolder> FetchControllerHolder::create()
{
    return GC::Heap::the().allocate<FetchControllerHolder>();
}

void FetchControllerHolder::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_controller);
}

}
