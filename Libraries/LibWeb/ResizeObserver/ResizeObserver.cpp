/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ResizeObserverPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::ResizeObserver {

GC_DEFINE_ALLOCATOR(ResizeObserver);

// https://drafts.csswg.org/resize-observer/#dom-resizeobserver-resizeobserver
WebIDL::ExceptionOr<GC::Ref<ResizeObserver>> ResizeObserver::construct_impl(JS::Realm& realm, WebIDL::CallbackType* callback)
{
    return realm.create<ResizeObserver>(realm, callback);
}

ResizeObserver::ResizeObserver(JS::Realm& realm, WebIDL::CallbackType* callback)
    : PlatformObject(realm)
    , m_callback(callback)
{
    auto navigable = as<HTML::Window>(HTML::relevant_global_object(*this)).navigable();
    m_document = navigable->active_document().ptr();
    m_document->register_resize_observer({}, *this);
}

ResizeObserver::~ResizeObserver() = default;

void ResizeObserver::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ResizeObserver);
}

void ResizeObserver::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_observation_targets);
    visitor.visit(m_active_targets);
    visitor.visit(m_skipped_targets);
}

void ResizeObserver::finalize()
{
    if (m_document)
        m_document->unregister_resize_observer({}, *this);
}

// https://drafts.csswg.org/resize-observer-1/#dom-resizeobserver-observe
void ResizeObserver::observe(DOM::Element& target, ResizeObserverOptions options)
{
    // 1. If target is in [[observationTargets]] slot, call unobserve() with argument target.
    auto observation = m_observation_targets.find_if([&](auto& observation) { return observation->target().ptr() == &target; });
    if (!observation.is_end())
        unobserve(target);

    // 2. Let observedBox be the value of the box dictionary member of options.
    auto observed_box = options.box;

    // 3. Let resizeObservation be new ResizeObservation(target, observedBox).
    auto resize_observation = MUST(ResizeObservation::create(realm(), target, observed_box));

    // 4. Add the resizeObservation to the [[observationTargets]] slot.
    m_observation_targets.append(resize_observation);
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
}

// https://drafts.csswg.org/resize-observer-1/#dom-resizeobserver-disconnect
void ResizeObserver::disconnect()
{
    // 1. Clear the [[observationTargets]] list.
    m_observation_targets.clear();

    // 2. Clear the [[activeTargets]] list.
    m_active_targets.clear();
}

void ResizeObserver::invoke_callback(ReadonlySpan<GC::Ref<ResizeObserverEntry>> entries) const
{
    auto& callback = *m_callback;
    auto& realm = callback.callback_context;

    auto wrapped_records = MUST(JS::Array::create(realm, 0));
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& record = entries.at(i);
        auto property_index = JS::PropertyKey { i };
        MUST(wrapped_records->create_data_property(property_index, record.ptr()));
    }

    (void)WebIDL::invoke_callback(callback, JS::js_undefined(), WebIDL::ExceptionBehavior::Report, wrapped_records);
}

}
