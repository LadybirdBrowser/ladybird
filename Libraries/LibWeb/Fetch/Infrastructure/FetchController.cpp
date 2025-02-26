/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(FetchController);

FetchController::FetchController() = default;

GC::Ref<FetchController> FetchController::create(JS::VM& vm)
{
    return vm.heap().allocate<FetchController>();
}

void FetchController::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_full_timing_info);
    visitor.visit(m_report_timing_steps);
    visitor.visit(m_next_manual_redirect_steps);
    visitor.visit(m_fetch_params);
}

void FetchController::set_report_timing_steps(Function<void(JS::Object&)> report_timing_steps)
{
    m_report_timing_steps = GC::create_function(vm().heap(), move(report_timing_steps));
}

void FetchController::set_next_manual_redirect_steps(Function<void()> next_manual_redirect_steps)
{
    m_next_manual_redirect_steps = GC::create_function(vm().heap(), move(next_manual_redirect_steps));
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

// https://fetch.spec.whatwg.org/#extract-full-timing-info
GC::Ref<FetchTimingInfo> FetchController::extract_full_timing_info() const
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

    // 4. Let serializedError be StructuredSerialize(error). If that threw an exception, catch it, and let serializedError be StructuredSerialize(fallbackError).
    // 5. Set controller’s serialized abort reason to serializedError
    auto structured_serialize = [](JS::VM& vm, JS::Value error, JS::Value fallback_error) {
        auto serialized_value_or_error = HTML::structured_serialize(vm, error);
        return serialized_value_or_error.is_error()
            ? HTML::structured_serialize(vm, fallback_error).value()
            : serialized_value_or_error.value();
    };
    m_serialized_abort_reason = structured_serialize(realm.vm(), error.value(), fallback_error);
}

// https://fetch.spec.whatwg.org/#deserialize-a-serialized-abort-reason
JS::Value FetchController::deserialize_a_serialized_abort_reason(JS::Realm& realm)
{
    // 1. Let fallbackError be an "AbortError" DOMException.
    auto fallback_error = WebIDL::AbortError::create(realm, "Fetch was aborted"_string);

    // 2. Let deserializedError be fallbackError.
    JS::Value deserialized_error = fallback_error;

    // 3. If abortReason is non-null, then set deserializedError to StructuredDeserialize(abortReason, realm).
    //    If that threw an exception or returned undefined, then set deserializedError to fallbackError.
    if (m_serialized_abort_reason.has_value()) {
        auto deserialized_error_or_exception = HTML::structured_deserialize(realm.vm(), m_serialized_abort_reason.value(), realm, {});
        if (!deserialized_error_or_exception.is_exception() && !deserialized_error_or_exception.value().is_undefined()) {
            deserialized_error = deserialized_error_or_exception.value();
        }
    }

    // 4. Return deserializedError.
    return deserialized_error;
}

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

GC_DEFINE_ALLOCATOR(FetchControllerHolder);

FetchControllerHolder::FetchControllerHolder() = default;

GC::Ref<FetchControllerHolder> FetchControllerHolder::create(JS::VM& vm)
{
    return vm.heap().allocate<FetchControllerHolder>();
}

void FetchControllerHolder::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_controller);
}

}
