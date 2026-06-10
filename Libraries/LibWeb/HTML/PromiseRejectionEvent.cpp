/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PromiseRejectionEvent.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PromiseRejectionEvent);

static PromiseRejectionEventInit promise_rejection_event_init_from_bindings(Bindings::PromiseRejectionEventInit const& event_init)
{
    return {
        {
            .bubbles = event_init.bubbles,
            .cancelable = event_init.cancelable,
            .composed = event_init.composed,
        },
        event_init.promise,
        event_init.reason.value_or(JS::js_undefined()),
    };
}

GC::Ref<PromiseRejectionEvent> PromiseRejectionEvent::create(JS::Object const& relevant_global_object, FlyString const& event_name, PromiseRejectionEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

GC::Ref<PromiseRejectionEvent> PromiseRejectionEvent::create(FlyString const& event_name, PromiseRejectionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<PromiseRejectionEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

WebIDL::ExceptionOr<GC::Ref<PromiseRejectionEvent>> PromiseRejectionEvent::create_for_constructor(JS::Realm& realm, FlyString const& event_name, Bindings::PromiseRejectionEventInit const& event_init)
{
    auto& global_scope = HTML::relevant_window_or_worker_global_scope(realm.global_object());
    return create(event_name, promise_rejection_event_init_from_bindings(event_init), HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope)));
}

PromiseRejectionEvent::PromiseRejectionEvent(FlyString const& event_name, PromiseRejectionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_promise(event_init.promise)
    , m_reason(event_init.reason)
{
}

PromiseRejectionEvent::~PromiseRejectionEvent() = default;

void PromiseRejectionEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_promise);
    visitor.visit(m_reason);
}

}
