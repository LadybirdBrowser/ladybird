/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/ResizeObserver/ResizeObservation.h>
#include <LibWeb/ResizeObserver/ResizeObserverEntry.h>

namespace Web::ResizeObserver {

struct ResizeObserverOptions {
    Bindings::ResizeObserverBoxOptions box;
};

// https://drafts.csswg.org/resize-observer-1/#resize-observer-interface
class ResizeObserver : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ResizeObserver, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(ResizeObserver);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<ResizeObserver>> construct_impl(JS::Realm&, WebIDL::CallbackType* callback);

    virtual ~ResizeObserver() override;

    void observe(DOM::Element& target, ResizeObserverOptions);
    void unobserve(DOM::Element& target);
    void disconnect();

    void invoke_callback(Vector<JS::NonnullGCPtr<ResizeObserverEntry>>& entries) const;

    Vector<JS::NonnullGCPtr<ResizeObservation>>& observation_targets() { return m_observation_targets; }
    Vector<JS::NonnullGCPtr<ResizeObservation>>& active_targets() { return m_active_targets; }
    Vector<JS::NonnullGCPtr<ResizeObservation>>& skipped_targets() { return m_skipped_targets; }

private:
    explicit ResizeObserver(JS::Realm&, WebIDL::CallbackType* callback);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    JS::GCPtr<WebIDL::CallbackType> m_callback;
    Vector<JS::NonnullGCPtr<ResizeObservation>> m_observation_targets;
    Vector<JS::NonnullGCPtr<ResizeObservation>> m_active_targets;
    Vector<JS::NonnullGCPtr<ResizeObservation>> m_skipped_targets;

    // AD-HOC: This is the document where we've registered the observer.
    WeakPtr<DOM::Document> m_document;
};

}
