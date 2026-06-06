/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ResizeObserver.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/ResizeObserver/ResizeObservation.h>
#include <LibWeb/ResizeObserver/ResizeObserverEntry.h>

namespace Web::ResizeObserver {

// https://drafts.csswg.org/resize-observer-1/#resize-observer-interface
class ResizeObserver : public Bindings::Wrappable {
    WEB_WRAPPABLE(ResizeObserver, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ResizeObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static WebIDL::ExceptionOr<GC::Ref<ResizeObserver>> construct_impl(HTML::Window&, WebIDL::CallbackType* callback);

    virtual ~ResizeObserver() override;

    void observe(DOM::Element& target, Bindings::ResizeObserverOptions);
    void unobserve(DOM::Element& target);
    void disconnect();

    void invoke_callback(ReadonlySpan<GC::Ref<ResizeObserverEntry>> entries);

    Vector<GC::Ref<ResizeObservation>>& observation_targets() { return m_observation_targets; }
    Vector<GC::Ref<ResizeObservation>>& active_targets() { return m_active_targets; }
    Vector<GC::Ref<ResizeObservation>>& skipped_targets() { return m_skipped_targets; }
    void remove_dead_observations();

private:
    explicit ResizeObserver(WebIDL::CallbackType* callback, DOM::Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

    void unregister_observer_if_needed();

    GC::Ptr<WebIDL::CallbackType> m_callback;
    Vector<GC::Ref<ResizeObservation>> m_observation_targets;
    Vector<GC::Ref<ResizeObservation>> m_active_targets;
    Vector<GC::Ref<ResizeObservation>> m_skipped_targets;

    // AD-HOC: This is the document where we've registered the observer.
    GC::Weak<DOM::Document> m_document;

    IntrusiveListNode<ResizeObserver> m_list_node;

public:
    using ResizeObserversList = IntrusiveList<&ResizeObserver::m_list_node>;
};

}
