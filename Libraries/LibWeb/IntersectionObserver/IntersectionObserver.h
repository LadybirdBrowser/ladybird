/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Handle.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IntersectionObserver/IntersectionObserverEntry.h>
#include <LibWeb/PixelUnits.h>

namespace Web::IntersectionObserver {

struct IntersectionObserverInit {
    Optional<Variant<JS::Handle<DOM::Element>, JS::Handle<DOM::Document>>> root;
    String root_margin { "0px"_string };
    Variant<double, Vector<double>> threshold { 0 };
};

// https://www.w3.org/TR/intersection-observer/#intersectionobserverregistration
struct IntersectionObserverRegistration {
    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverregistration-observer
    // [A]n observer property holding an IntersectionObserver.
    JS::NonnullGCPtr<IntersectionObserver> observer;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverregistration-observer
    // NOTE: Optional is used in place of the spec using -1 to indicate no previous index.
    // [A] previousThresholdIndex property holding a number between -1 and the length of the observer’s thresholds property (inclusive).
    Optional<size_t> previous_threshold_index;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverregistration-previousisintersecting
    // [A] previousIsIntersecting property holding a boolean.
    bool previous_is_intersecting { false };
};

// https://w3c.github.io/IntersectionObserver/#intersection-observer-interface
class IntersectionObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IntersectionObserver, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(IntersectionObserver);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<IntersectionObserver>> construct_impl(JS::Realm&, JS::GCPtr<WebIDL::CallbackType> callback, IntersectionObserverInit const& options = {});

    virtual ~IntersectionObserver() override;

    void observe(DOM::Element& target);
    void unobserve(DOM::Element& target);
    void disconnect();
    Vector<JS::Handle<IntersectionObserverEntry>> take_records();

    Vector<JS::NonnullGCPtr<DOM::Element>> const& observation_targets() const { return m_observation_targets; }

    Variant<JS::Handle<DOM::Element>, JS::Handle<DOM::Document>, Empty> root() const;
    Vector<double> const& thresholds() const { return m_thresholds; }

    Variant<JS::Handle<DOM::Element>, JS::Handle<DOM::Document>> intersection_root() const;
    CSSPixelRect root_intersection_rectangle() const;

    void queue_entry(Badge<DOM::Document>, JS::NonnullGCPtr<IntersectionObserverEntry>);

    WebIDL::CallbackType& callback() { return *m_callback; }

private:
    explicit IntersectionObserver(JS::Realm&, JS::GCPtr<WebIDL::CallbackType> callback, Optional<Variant<JS::Handle<DOM::Element>, JS::Handle<DOM::Document>>> const& root, Vector<double>&& thresholds);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void finalize() override;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserver-callback-slot
    JS::GCPtr<WebIDL::CallbackType> m_callback;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserver-root
    JS::GCPtr<DOM::Node> m_root;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserver-thresholds
    Vector<double> m_thresholds;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserver-queuedentries-slot
    Vector<JS::NonnullGCPtr<IntersectionObserverEntry>> m_queued_entries;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserver-observationtargets-slot
    Vector<JS::NonnullGCPtr<DOM::Element>> m_observation_targets;

    // AD-HOC: This is the document where we've registered the IntersectionObserver.
    WeakPtr<DOM::Document> m_document;
};

}
