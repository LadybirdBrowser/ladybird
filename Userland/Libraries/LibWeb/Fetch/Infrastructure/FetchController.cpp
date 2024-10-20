/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Fetch::Infrastructure {

JS_DEFINE_ALLOCATOR(FetchController);

FetchController::FetchController() = default;

JS::NonnullGCPtr<FetchController> FetchController::create(JS::VM& vm)
{
    return vm.heap().allocate_without_realm<FetchController>();
}

void FetchController::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_full_timing_info);
    visitor.visit(m_report_timing_steps);
    visitor.visit(m_next_manual_redirect_steps);
    visitor.visit(m_fetch_params);
}

void FetchController::set_report_timing_steps(Function<void(JS::Object const&)> report_timing_steps)
{
    m_report_timing_steps = JS::create_heap_function(vm().heap(), move(report_timing_steps));
}

void FetchController::set_next_manual_redirect_steps(Function<void()> next_manual_redirect_steps)
{
    m_next_manual_redirect_steps = JS::create_heap_function(vm().heap(), move(next_manual_redirect_steps));
}

// https://fetch.spec.whatwg.org/#finalize-and-report-timing
void FetchController::report_timing(JS::Object const& global) const
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

// https://fetch.spec.whatwg.org/#extract-full-timing-info
JS::NonnullGCPtr<FetchTimingInfo> FetchController::extract_full_timing_info() const
{
    // 1. Assert: this’s full timing info is not null.
    VERIFY(m_full_timing_info);

    // 2. Return this’s full timing info.
    return *m_full_timing_info;
}

// https://fetch.spec.whatwg.org/#fetch-controller-abort
void FetchController::abort(JS::Realm& realm, Optional<JS::Value> error)
{
    // 1. Set controller’s state to "aborted".
    m_state = State::Aborted;

    // 2. Let fallbackError be an "AbortError" DOMException.
    auto fallback_error = WebIDL::AbortError::create(realm, "Fetch was aborted"_string);

    // 3. Set error to fallbackError if it is not given.
    if (!error.has_value())
        error = fallback_error;

    // FIXME: 4. Let serializedError be StructuredSerialize(error). If that threw an exception, catch it, and let serializedError be StructuredSerialize(fallbackError).
    // FIXME: 5. Set controller’s serialized abort reason to serializedError.
    (void)error;
}

// FIXME: https://fetch.spec.whatwg.org/#deserialize-a-serialized-abort-reason

// https://fetch.spec.whatwg.org/#fetch-controller-terminate
void FetchController::terminate()
{
    // To terminate a fetch controller controller, set controller’s state to "terminated".
    m_state = State::Terminated;
}

void FetchController::stop_fetch()
{
    auto& vm = this->vm();

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

    if (m_fetch_params) {
        auto fetch_algorithms = FetchAlgorithms::create(vm, {});
        m_fetch_params->set_algorithms(fetch_algorithms);
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

}
