/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/ResizeObserverEntry.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::ResizeObserver {

GC_DEFINE_ALLOCATOR(ResizeObserver);

void invoke_resize_observer_callback(ResizeObserver& observer, ReadonlySpan<GC::Ref<ResizeObserverEntry>> entries)
{
    auto& callback = observer.callback();
    auto& callback_realm = callback.callback_context->realm();
    auto& wrapper_world = Bindings::host_defined_wrapper_world(callback_realm);

    auto wrapped_records = MUST(JS::Array::create(callback_realm, 0));
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& record = entries.at(i);
        auto property_index = JS::PropertyKey { i };
        MUST(wrapped_records->create_data_property(property_index, Bindings::wrap(wrapper_world, callback_realm, record)));
    }

    auto wrapped_observer = Bindings::wrap(wrapper_world, callback_realm, GC::Ref { observer });
    (void)WebIDL::invoke_callback(callback, wrapped_observer, WebIDL::ExceptionBehavior::Report, { { wrapped_records, wrapped_observer } });
}

// https://drafts.csswg.org/resize-observer/#dom-resizeobserver-resizeobserver
GC::Ref<ResizeObserver> ResizeObserver::create(WebIDL::CallbackType* callback, DOM::Document& document)
{
    return GC::Heap::the().allocate<ResizeObserver>(callback, document);
}

GC::Ref<ResizeObserver> ResizeObserver::create_for_constructor(JS::Realm& realm, WebIDL::CallbackType* callback)
{
    auto& window = HTML::relevant_window(realm.global_object());
    return create(callback, window.associated_document());
}

ResizeObserver::ResizeObserver(WebIDL::CallbackType* callback, DOM::Document& document)
    : m_callback(callback)
{
    m_document = document;
}

ResizeObserver::~ResizeObserver() = default;

void ResizeObserver::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_observation_targets);
    visitor.visit(m_active_targets);
    visitor.visit(m_skipped_targets);
}

void ResizeObserver::finalize()
{
    Base::finalize();
    if (m_document && m_list_node.is_in_list())
        m_document->unregister_resize_observer({}, *this);
}

void ResizeObserver::observe(DOM::Element& target, ResizeObserverOptions options)
{
    // 1. If target is in [[observationTargets]] slot, call unobserve() with argument target.
    auto observation = m_observation_targets.find_if([&](auto& observation) { return observation->target().ptr() == &target; });
    if (!observation.is_end())
        unobserve(target);

    // 2. Let observedBox be the value of the box dictionary member of options.
    auto observed_box = options.box;

    // 3. Let resizeObservation be new ResizeObservation(target, observedBox).
    auto resize_observation = MUST(ResizeObservation::create(target, observed_box));

    // 4. Add the resizeObservation to the [[observationTargets]] slot.
    m_observation_targets.append(resize_observation);

    if (!m_list_node.is_in_list()) {
        m_document->register_resize_observer({}, *this);
    }

    m_document->page().client().request_frame();
}

// https://drafts.csswg.org/resize-observer-1/#dom-resizeobserver-unobserve
void ResizeObserver::unobserve(DOM::Element& target)
{
    // 1. Let observation be ResizeObservation in [[observationTargets]] whose target slot is target.
    auto observation = m_observation_targets.find_if([&](auto& observation) { return observation->target().ptr() == &target; });

    // 2. If observation is not found, return.
    if (observation.is_end())
        return;

    // 3. Remove observation from [[observationTargets]].
    m_observation_targets.remove(observation.index());

    unregister_observer_if_needed();
}

// https://drafts.csswg.org/resize-observer-1/#dom-resizeobserver-disconnect
void ResizeObserver::disconnect()
{
    // 1. Clear the [[observationTargets]] list.
    m_observation_targets.clear();

    // 2. Clear the [[activeTargets]] list.
    m_active_targets.clear();

    unregister_observer_if_needed();
}

void ResizeObserver::remove_dead_observations()
{
    m_observation_targets.remove_all_matching([](auto& observation) {
        return !observation->target();
    });
    m_active_targets.remove_all_matching([](auto& observation) {
        return !observation->target();
    });
    m_skipped_targets.remove_all_matching([](auto& observation) {
        return !observation->target();
    });

    unregister_observer_if_needed();
}

void ResizeObserver::unregister_observer_if_needed()
{
    // https://drafts.csswg.org/resize-observer/#lifetime
    // A ResizeObserver will remain alive until both of these conditions are met:
    // - there are no scripting references to the observer.
    // - the observer is not observing any targets.

    // The first condition from the spec is handled by visiting ResizeObserver from
    // JS environment that holds a reference to ResizeObserver.
    // Here we handle the second condition.
    if (m_observation_targets.is_empty() && m_list_node.is_in_list() && m_document) {
        m_document->unregister_resize_observer({}, *this);
    }
}

}
