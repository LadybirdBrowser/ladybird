/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Bindings/IntersectionObserverPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Page/Page.h>

namespace Web::IntersectionObserver {

JS_DEFINE_ALLOCATOR(IntersectionObserver);

// https://w3c.github.io/IntersectionObserver/#dom-intersectionobserver-intersectionobserver
WebIDL::ExceptionOr<JS::NonnullGCPtr<IntersectionObserver>> IntersectionObserver::construct_impl(JS::Realm& realm, JS::GCPtr<WebIDL::CallbackType> callback, IntersectionObserverInit const& options)
{
    // 4. Let thresholds be a list equal to options.threshold.
    Vector<double> thresholds;
    if (options.threshold.has<double>()) {
        thresholds.append(options.threshold.get<double>());
    } else {
        VERIFY(options.threshold.has<Vector<double>>());
        thresholds = options.threshold.get<Vector<double>>();
    }

    // 5. If any value in thresholds is less than 0.0 or greater than 1.0, throw a RangeError exception.
    for (auto value : thresholds) {
        if (value < 0.0 || value > 1.0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Threshold values must be between 0.0 and 1.0 inclusive"sv };
    }

    // 6. Sort thresholds in ascending order.
    quick_sort(thresholds, [](double left, double right) {
        return left < right;
    });

    // 1. Let this be a new IntersectionObserver object
    // 2. Set this’s internal [[callback]] slot to callback.
    // 8. The thresholds attribute getter will return this sorted thresholds list.
    // 9. Return this.
    return realm.heap().allocate<IntersectionObserver>(realm, realm, callback, options.root, move(thresholds));
}

IntersectionObserver::IntersectionObserver(JS::Realm& realm, JS::GCPtr<WebIDL::CallbackType> callback, Optional<Variant<JS::Handle<DOM::Element>, JS::Handle<DOM::Document>>> const& root, Vector<double>&& thresholds)
    : PlatformObject(realm)
    , m_callback(callback)
    , m_thresholds(move(thresholds))
{
    m_root = root.has_value() ? root->visit([](auto& value) -> JS::GCPtr<DOM::Node> { return *value; }) : nullptr;
    intersection_root().visit([this](auto& node) {
        m_document = node->document();
    });
    m_document->register_intersection_observer({}, *this);
}

IntersectionObserver::~IntersectionObserver() = default;

void IntersectionObserver::finalize()
{
    if (m_document)
        m_document->unregister_intersection_observer({}, *this);
}

void IntersectionObserver::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IntersectionObserver);
}

void IntersectionObserver::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_root);
    visitor.visit(m_callback);
    visitor.visit(m_queued_entries);
    visitor.visit(m_observation_targets);
}

// https://w3c.github.io/IntersectionObserver/#dom-intersectionobserver-observe
void IntersectionObserver::observe(DOM::Element& target)
{
    // Run the observe a target Element algorithm, providing this and target.
    // https://www.w3.org/TR/intersection-observer/#observe-a-target-element
    // 1. If target is in observer’s internal [[ObservationTargets]] slot, return.
    if (m_observation_targets.contains_slow(JS::NonnullGCPtr { target }))
        return;

    // 2. Let intersectionObserverRegistration be an IntersectionObserverRegistration record with an observer
    //    property set to observer, a previousThresholdIndex property set to -1, and a previousIsIntersecting
    //    property set to false.
    auto intersection_observer_registration = IntersectionObserverRegistration {
        .observer = *this,
        .previous_threshold_index = OptionalNone {},
        .previous_is_intersecting = false,
    };

    // 3. Append intersectionObserverRegistration to target’s internal [[RegisteredIntersectionObservers]] slot.
    target.register_intersection_observer({}, move(intersection_observer_registration));

    // 4. Add target to observer’s internal [[ObservationTargets]] slot.
    m_observation_targets.append(target);
}

// https://w3c.github.io/IntersectionObserver/#dom-intersectionobserver-unobserve
void IntersectionObserver::unobserve(DOM::Element& target)
{
    // Run the unobserve a target Element algorithm, providing this and target.
    // https://www.w3.org/TR/intersection-observer/#unobserve-a-target-element
    // 1. Remove the IntersectionObserverRegistration record whose observer property is equal to this from target’s internal [[RegisteredIntersectionObservers]] slot, if present.
    target.unregister_intersection_observer({}, *this);

    // 2. Remove target from this’s internal [[ObservationTargets]] slot, if present
    m_observation_targets.remove_first_matching([&target](JS::NonnullGCPtr<DOM::Element> const& entry) {
        return entry.ptr() == &target;
    });
}

// https://w3c.github.io/IntersectionObserver/#dom-intersectionobserver-disconnect
void IntersectionObserver::disconnect()
{
    // For each target in this’s internal [[ObservationTargets]] slot:
    // 1. Remove the IntersectionObserverRegistration record whose observer property is equal to this from target’s internal
    //    [[RegisteredIntersectionObservers]] slot.
    // 2. Remove target from this’s internal [[ObservationTargets]] slot.
    for (auto& target : m_observation_targets) {
        target->unregister_intersection_observer({}, *this);
    }
    m_observation_targets.clear();
}

// https://www.w3.org/TR/intersection-observer/#dom-intersectionobserver-takerecords
Vector<JS::Handle<IntersectionObserverEntry>> IntersectionObserver::take_records()
{
    // 1. Let queue be a copy of this’s internal [[QueuedEntries]] slot.
    Vector<JS::Handle<IntersectionObserverEntry>> queue;
    for (auto& entry : m_queued_entries)
        queue.append(*entry);

    // 2. Clear this’s internal [[QueuedEntries]] slot.
    m_queued_entries.clear();

    // 3. Return queue.
    return queue;
}

Variant<JS::Handle<DOM::Element>, JS::Handle<DOM::Document>, Empty> IntersectionObserver::root() const
{
    if (!m_root)
        return Empty {};
    if (m_root->is_element())
        return JS::make_handle(static_cast<DOM::Element&>(*m_root));
    if (m_root->is_document())
        return JS::make_handle(static_cast<DOM::Document&>(*m_root));
    VERIFY_NOT_REACHED();
}

// https://www.w3.org/TR/intersection-observer/#intersectionobserver-intersection-root
Variant<JS::Handle<DOM::Element>, JS::Handle<DOM::Document>> IntersectionObserver::intersection_root() const
{
    // The intersection root for an IntersectionObserver is the value of its root attribute
    // if the attribute is non-null;
    if (m_root) {
        if (m_root->is_element())
            return JS::make_handle(static_cast<DOM::Element&>(*m_root));
        if (m_root->is_document())
            return JS::make_handle(static_cast<DOM::Document&>(*m_root));
        VERIFY_NOT_REACHED();
    }

    // otherwise, it is the top-level browsing context’s document node, referred to as the implicit root.
    return JS::make_handle(verify_cast<HTML::Window>(HTML::relevant_global_object(*this)).page().top_level_browsing_context().active_document());
}

// https://www.w3.org/TR/intersection-observer/#intersectionobserver-root-intersection-rectangle
CSSPixelRect IntersectionObserver::root_intersection_rectangle() const
{
    // If the IntersectionObserver is an implicit root observer,
    //    it’s treated as if the root were the top-level browsing context’s document, according to the following rule for document.
    auto intersection_root = this->intersection_root();

    CSSPixelRect rect;

    // If the intersection root is a document,
    //    it’s the size of the document's viewport (note that this processing step can only be reached if the document is fully active).
    if (intersection_root.has<JS::Handle<DOM::Document>>()) {
        auto document = intersection_root.get<JS::Handle<DOM::Document>>();

        // Since the spec says that this is only reach if the document is fully active, that means it must have a navigable.
        VERIFY(document->navigable());

        // NOTE: This rect is the *size* of the viewport. The viewport *offset* is not relevant,
        //       as intersections are computed using viewport-relative element rects.
        rect = CSSPixelRect { CSSPixelPoint { 0, 0 }, document->viewport_rect().size() };
    } else {
        VERIFY(intersection_root.has<JS::Handle<DOM::Element>>());
        auto element = intersection_root.get<JS::Handle<DOM::Element>>();

        // FIXME: Otherwise, if the intersection root has a content clip,
        //          it’s the element’s content area.

        // Otherwise,
        //    it’s the result of getting the bounding box for the intersection root.
        auto bounding_client_rect = element->get_bounding_client_rect();
        rect = CSSPixelRect(bounding_client_rect->x(), bounding_client_rect->y(), bounding_client_rect->width(), bounding_client_rect->height());
    }

    // FIXME: When calculating the root intersection rectangle for a same-origin-domain target, the rectangle is then
    //        expanded according to the offsets in the IntersectionObserver’s [[rootMargin]] slot in a manner similar
    //        to CSS’s margin property, with the four values indicating the amount the top, right, bottom, and left
    //        edges, respectively, are offset by, with positive lengths indicating an outward offset. Percentages
    //        are resolved relative to the width of the undilated rectangle.

    return rect;
}

void IntersectionObserver::queue_entry(Badge<DOM::Document>, JS::NonnullGCPtr<IntersectionObserverEntry> entry)
{
    m_queued_entries.append(entry);
}

}
