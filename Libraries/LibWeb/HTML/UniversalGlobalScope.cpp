/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

UniversalGlobalScopeMixin::~UniversalGlobalScopeMixin() = default;

static JS::Realm& relevant_realm_for_universal_global_scope(DOM::EventTarget& global)
{
    if (auto* worklet_global_scope = as_if<WorkletGlobalScope>(global))
        return worklet_global_scope->realm();
    return relevant_realm(relevant_window_or_worker_global_scope(global));
}

void UniversalGlobalScopeMixin::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.ignore(m_outstanding_rejected_promises_weak_set);
    visitor.visit(m_about_to_be_notified_rejected_promises_list);
}

void UniversalGlobalScopeMixin::push_onto_outstanding_rejected_promises_weak_set(GC::Ptr<JS::Promise> promise)
{
    m_outstanding_rejected_promises_weak_set.append(promise);
}

bool UniversalGlobalScopeMixin::remove_from_outstanding_rejected_promises_weak_set(GC::Ptr<JS::Promise> promise)
{
    return m_outstanding_rejected_promises_weak_set.remove_first_matching([&](GC::Ptr<JS::Promise> promise_in_set) {
        return promise == promise_in_set;
    });
}

void UniversalGlobalScopeMixin::push_onto_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise> promise)
{
    if (!m_about_to_be_notified_rejected_promises_list)
        m_about_to_be_notified_rejected_promises_list = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<JS::Promise>>>();
    m_about_to_be_notified_rejected_promises_list->elements().append(promise);
}

bool UniversalGlobalScopeMixin::remove_from_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise> promise)
{
    if (!m_about_to_be_notified_rejected_promises_list)
        return false;
    return m_about_to_be_notified_rejected_promises_list->elements().remove_first_matching([&](auto& promise_in_list) {
        return promise == promise_in_list;
    });
}

// https://html.spec.whatwg.org/multipage/webappapis.html#notify-about-rejected-promises
void UniversalGlobalScopeMixin::notify_about_rejected_promises(Badge<EventLoop>)
{
    // 1. Let list be a copy of settings object's about-to-be-notified rejected promises list.
    auto list = m_about_to_be_notified_rejected_promises_list;

    // 2. If list is empty, return.
    if (!list || list->elements().is_empty())
        return;

    // 3. Clear settings object's about-to-be-notified rejected promises list.
    m_about_to_be_notified_rejected_promises_list = nullptr;

    // 4. Let global be settings object's global object.
    auto& global = this_impl();
    auto& global_object = relevant_realm_for_universal_global_scope(global).global_object();

    // 5. Queue a global task on the DOM manipulation task source given global to run the following substep:
    queue_global_task(Task::Source::DOMManipulation, global_object, GC::create_function(GC::Heap::the(), [this, &global, list = move(list)] {
        auto& realm = relevant_realm_for_universal_global_scope(global);

        // 1. For each promise p in list:
        for (auto const& promise : list->elements()) {

            // 1. If p's [[PromiseIsHandled]] internal slot is true, continue to the next iteration of the loop.
            if (promise->is_handled())
                continue;

            // 2. Let notHandled be the result of firing an event named unhandledrejection at global, using PromiseRejectionEvent, with the cancelable attribute initialized to true,
            //    the promise attribute initialized to p, and the reason attribute initialized to the value of p's [[PromiseResult]] internal slot.
            PromiseRejectionEventInit event_init {
                {
                    .bubbles = false,
                    .cancelable = true,
                    .composed = false,
                },
                // Sadly we can't use .promise and .reason here, as we can't use the designator on the initialization of EventInit above.
                /* .promise = */ *promise,
                /* .reason = */ promise->result(),
            };

            auto promise_rejection_event = PromiseRejectionEvent::create(realm.global_object(), HTML::EventNames::unhandledrejection, event_init);

            bool not_handled = global.dispatch_event(*promise_rejection_event);

            // 3. If notHandled is false, then the promise rejection is handled. Otherwise, the promise rejection is not handled.

            // 4. If p's [[PromiseIsHandled]] internal slot is false, add p to settings object's outstanding rejected promises weak set.
            if (!promise->is_handled())
                m_outstanding_rejected_promises_weak_set.append(*promise);

            // This algorithm results in promise rejections being marked as handled or not handled. These concepts parallel handled and not handled script errors.
            // If a rejection is still not handled after this, then the rejection may be reported to a developer console.
            if (not_handled)
                HTML::report_exception_to_console(promise->result(), realm, ErrorInPromise::Yes);
        }
    }));
}

static bool s_experimental_interfaces_exposed = false;

void UniversalGlobalScopeMixin::set_experimental_interfaces_exposed(bool exposed)
{
    s_experimental_interfaces_exposed = exposed;
}

bool UniversalGlobalScopeMixin::expose_experimental_interfaces()
{
    return s_experimental_interfaces_exposed;
}

}
