/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineEvent.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/Network.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Fetch::Infrastructure {

extern "C" {

// Called by Bindings/Rust/src/fetch/infrastructure/fetch_controller.rs.
FetchController* web_fetch_controller_create(JSObject* global_object);
void web_fetch_controller_terminate(FetchController*);
void web_fetch_controller_abort(FetchController*, void* realm);
bool web_fetch_controller_has_report_timing_steps(FetchController*);
void web_fetch_controller_report_timing(FetchController*, JSObject* global_object);
}

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
    visitor.visit(m_realm);
    visitor.visit(m_report_timing_steps);
    if (m_awaited_end_of_body.has_value())
        visitor.visit(m_awaited_end_of_body->steps);
    visitor.visit(m_next_manual_redirect_steps);
    visitor.visit(m_algorithms);
    visitor.visit(m_task_destination);
    visitor.visit(m_page);
    visitor.visit(m_stop_steps);
}

void FetchController::finalize()
{
    // A discarded response can leave an ongoing controller without a stop or end-of-body task.
    end_rendering_pending_operation(this);
    Base::finalize();
    if (m_registry)
        m_registry->remove({}, m_token);
}

NonnullRefPtr<FetchParams> FetchController::create_fetch_params(NonnullRefPtr<Request> request, GC::Ref<FetchAlgorithms const> algorithms, NonnullRefPtr<FetchTimingInfo> timing_info, NonnullRefPtr<LoaderConfigSnapshot const> loader_config)
{
    VERIFY(!m_algorithms);
    VERIFY(!m_request);
    VERIFY(!m_network_observation);

    auto fetch_params = FetchParams::create(m_token, m_registry, m_control_block, request, move(timing_info), move(loader_config));
    m_algorithms = algorithms;
    m_control_block->set_body_intent({}, algorithms->body_intent());
    m_original_fetch_params = fetch_params;
    m_network_observation = request->ensure_network_observation();
    m_request = move(request);
    return fetch_params;
}

void FetchController::retarget(TaskDestination task_destination, Engine::BodyIntent body_intent)
{
    m_task_destination = move(task_destination);
    m_control_block->set_body_intent({}, body_intent);
}

void FetchController::report_webdriver_bidi_fetch_error(StringView error_text) const
{
    if (!WebDriver::BiDi::network_events_enabled())
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

NonnullRefPtr<Request> FetchController::request() const
{
    VERIFY(m_request);
    return *m_request;
}

GC::Ref<FetchAlgorithms const> FetchController::algorithms() const
{
    VERIFY(m_algorithms);
    return *m_algorithms;
}

GC::Ref<Page> FetchController::page() const
{
    VERIFY(m_page);
    return *m_page;
}

void FetchController::set_report_timing_steps(GC::Ref<GC::Function<void(JS::Object&)>> report_timing_steps)
{
    m_report_timing_steps = report_timing_steps;
}

void FetchController::set_next_manual_redirect_steps(GC::Ref<GC::Function<void()>> next_manual_redirect_steps)
{
    m_next_manual_redirect_steps = next_manual_redirect_steps;
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

void FetchController::resolve_redirect(Engine::Command::ResolveRedirect::Resolution resolution)
{
    auto continuation_id = exchange(m_redirect_continuation_id, {});
    if (!continuation_id.has_value())
        return;

    VERIFY(m_started_fetch.has_value());
    Engine::Command::ResolveRedirect resolve {
        .fetch_id = m_started_fetch->fetch_id,
        .continuation_id = *continuation_id,
        .resolution = move(resolution),
    };
    m_started_fetch->fetch_executor->post(move(resolve));
}

void FetchController::when_body_ends(Engine::DeliveryId delivery_id, GC::Ptr<GC::Function<void(HighResolutionTime::DOMHighResTimeStamp)>> steps)
{
    if (m_fetch_ended)
        return;
    m_awaited_end_of_body = AwaitedEndOfBody { delivery_id, steps };
}

void FetchController::did_end_body(Engine::Event::EndOfBody const& end_of_body)
{
    if (!m_awaited_end_of_body.has_value() || m_awaited_end_of_body->delivery_id != end_of_body.delivery_id)
        return;
    auto awaited = m_awaited_end_of_body.release_value();
    if (awaited.steps)
        awaited.steps->function()(end_of_body.end_time);
}

void FetchController::did_end_fetch()
{
    m_fetch_ended = true;
    m_awaited_end_of_body.clear();

    VERIFY(m_started_fetch.has_value());
    if (auto* registry = m_registry.ptr())
        registry->release_fetch(m_started_fetch->fetch_id);
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

void FetchController::set_started_fetch(Engine::FetchId fetch_id, Engine::FetchExecutor& fetch_executor)
{
    VERIFY(!m_started_fetch.has_value());
    m_started_fetch = StartedFetch { fetch_id, fetch_executor };
}

Optional<Engine::FetchId> FetchController::started_fetch_id() const
{
    if (!m_started_fetch.has_value())
        return {};
    return m_started_fetch->fetch_id;
}

Engine::FetchExecutor& FetchController::started_fetch_executor() const
{
    VERIFY(m_started_fetch.has_value());
    return *m_started_fetch->fetch_executor;
}

void FetchController::stop_fetch()
{
    if (m_control_block->is_canceled())
        return;

    m_control_block->set_state({}, State::Stopped);
    report_webdriver_bidi_fetch_error("Fetch was stopped"sv);
    cancel_webdriver_bidi_network_interception();

    // AD-HOC: Some HTML elements need to stop an ongoing fetching process without causing any network error to be raised
    //         (which abort() and terminate() will both do). This is tricky because the fetch process runs across several
    //         nested Platform::EventLoopPlugin::deferred_invoke() invocations. For now, we "stop" the fetch process by
    //         cancelling any queued fetch tasks and then ignoring any callbacks.
    auto ongoing_fetch_tasks = move(m_ongoing_fetch_tasks);

    HTML::current_event_loop().task_queue().remove_tasks_matching([&](auto const& task) {
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

static Engine::EngineCommand stop_command(FetchControlBlock::State state, Engine::FetchId fetch_id)
{
    switch (state) {
    case FetchControlBlock::State::Aborted:
        return Engine::Command::Abort { fetch_id };
    case FetchControlBlock::State::Terminated:
        return Engine::Command::Terminate { fetch_id };
    case FetchControlBlock::State::Stopped:
        return Engine::Command::Stop { fetch_id };
    case FetchControlBlock::State::Ongoing:
        break;
    }
    VERIFY_NOT_REACHED();
}

// Posts the command that stops the network request of a fetch that is no longer ongoing, to the executor the fetch was
// started on.
void FetchController::stop_request()
{
    end_rendering_pending_operation(this);
    VERIFY(!m_control_block->is_ongoing());
    m_awaited_end_of_body.clear();

    if (!m_started_fetch.has_value())
        return;
    m_started_fetch->fetch_executor->post(stop_command(state(), m_started_fetch->fetch_id));
}

void FetchController::fetch_task_queued(u64 fetch_task_id, HTML::TaskID event_id)
{
    m_ongoing_fetch_tasks.set(fetch_task_id, event_id);
}

void FetchController::fetch_task_complete(u64 fetch_task_id)
{
    m_ongoing_fetch_tasks.remove(fetch_task_id);
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

FetchController* web_fetch_controller_create(JSObject* global_object)
{
    auto& relevant_agent = HTML::relevant_agent(*JS::HostABI::object_from_abi(global_object));
    return FetchController::create(relevant_agent.fetch_controller_registry).ptr();
}

void web_fetch_controller_terminate(FetchController* controller)
{
    controller->terminate();
}

void web_fetch_controller_abort(FetchController* controller, void* realm)
{
    controller->abort(*static_cast<JS::Realm*>(realm), {});
}

bool web_fetch_controller_has_report_timing_steps(FetchController* controller)
{
    return controller->has_report_timing_steps();
}

void web_fetch_controller_report_timing(FetchController* controller, JSObject* global_object)
{
    controller->report_timing(*JS::HostABI::object_from_abi(global_object));
}

}
