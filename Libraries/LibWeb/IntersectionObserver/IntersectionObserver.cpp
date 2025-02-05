/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Bindings/IntersectionObserverPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Page/Page.h>

namespace Web::IntersectionObserver {

GC_DEFINE_ALLOCATOR(IntersectionObserver);

// https://w3c.github.io/IntersectionObserver/#dom-intersectionobserver-intersectionobserver
WebIDL::ExceptionOr<GC::Ref<IntersectionObserver>> IntersectionObserver::construct_impl(JS::Realm& realm, GC::Ptr<WebIDL::CallbackType> callback, IntersectionObserverInit const& options)
{
    // https://w3c.github.io/IntersectionObserver/#initialize-a-new-intersectionobserver
    // 1. Let this be a new IntersectionObserver object
    // 2. Set this’s internal [[callback]] slot to callback.
    // NOTE: Steps 1 and 2 are handled by creating the IntersectionObserver at the very end of this function.

    // 3. Attempt to parse a margin from options.rootMargin. If a list is returned, set this’s internal [[rootMargin]] slot to that. Otherwise, throw a SyntaxError exception.
    auto root_margin = parse_a_margin(realm, options.root_margin);
    if (!root_margin.has_value()) {
        return WebIDL::SyntaxError::create(realm, "IntersectionObserver: Cannot parse root margin as a margin."_string);
    }

    // 4. Attempt to parse a margin from options.scrollMargin. If a list is returned, set this’s internal [[scrollMargin]] slot to that. Otherwise, throw a SyntaxError exception.
    auto scroll_margin = parse_a_margin(realm, options.scroll_margin);
    if (!scroll_margin.has_value()) {
        return WebIDL::SyntaxError::create(realm, "IntersectionObserver: Cannot parse scroll margin as a margin."_string);
    }

    // 5. Let thresholds be a list equal to options.threshold.
    Vector<double> thresholds;
    if (options.threshold.has<double>()) {
        thresholds.append(options.threshold.get<double>());
    } else {
        VERIFY(options.threshold.has<Vector<double>>());
        thresholds = options.threshold.get<Vector<double>>();
    }

    // 6. If any value in thresholds is less than 0.0 or greater than 1.0, throw a RangeError exception.
    for (auto value : thresholds) {
        if (value < 0.0 || value > 1.0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Threshold values must be between 0.0 and 1.0 inclusive"sv };
    }

    // 7. Sort thresholds in ascending order.
    quick_sort(thresholds, [](double left, double right) {
        return left < right;
    });

    // 8. If thresholds is empty, append 0 to thresholds.
    if (thresholds.is_empty()) {
        thresholds.append(0);
    }

    // 9. The thresholds attribute getter will return this sorted thresholds list.
    // NOTE: Handled implicitly by passing it into the constructor at the end of this function

    // 10. Let delay be the value of options.delay.
    auto delay = options.delay;

    // 11. If options.trackVisibility is true and delay is less than 100, set delay to 100.
    if (options.track_visibility && delay < 100) {
        delay = 100;
    }

    // 12. Set this’s internal [[delay]] slot to options.delay to delay.
    // 13. Set this’s internal [[trackVisibility]] slot to options.trackVisibility.
    // 14. Return this.
    return realm.create<IntersectionObserver>(realm, callback, options.root, move(root_margin.value()), move(scroll_margin.value()), move(thresholds), move(delay), move(options.track_visibility));
}

IntersectionObserver::IntersectionObserver(JS::Realm& realm, GC::Ptr<WebIDL::CallbackType> callback, Optional<Variant<GC::Root<DOM::Element>, GC::Root<DOM::Document>>> const& root, Vector<CSS::LengthPercentage> root_margin, Vector<CSS::LengthPercentage> scroll_margin, Vector<double>&& thresholds, double delay, bool track_visibility)
    : PlatformObject(realm)
    , m_callback(callback)
    , m_root_margin(root_margin)
    , m_scroll_margin(scroll_margin)
    , m_thresholds(move(thresholds))
    , m_delay(delay)
    , m_track_visibility(track_visibility)
{
    m_root = root.has_value() ? root->visit([](auto& value) -> GC::Ptr<DOM::Node> { return *value; }) : nullptr;
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
    if (m_observation_targets.contains_slow(GC::Ref { target }))
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
    m_observation_targets.remove_first_matching([&target](GC::Ref<DOM::Element> const& entry) {
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
Vector<GC::Root<IntersectionObserverEntry>> IntersectionObserver::take_records()
{
    // 1. Let queue be a copy of this’s internal [[QueuedEntries]] slot.
    Vector<GC::Root<IntersectionObserverEntry>> queue;
    for (auto& entry : m_queued_entries)
        queue.append(*entry);

    // 2. Clear this’s internal [[QueuedEntries]] slot.
    m_queued_entries.clear();

    // 3. Return queue.
    return queue;
}

Variant<GC::Root<DOM::Element>, GC::Root<DOM::Document>, Empty> IntersectionObserver::root() const
{
    if (!m_root)
        return Empty {};
    if (m_root->is_element())
        return GC::make_root(static_cast<DOM::Element&>(*m_root));
    if (m_root->is_document())
        return GC::make_root(static_cast<DOM::Document&>(*m_root));
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/IntersectionObserver/#dom-intersectionobserver-rootmargin
String IntersectionObserver::root_margin() const
{
    // On getting, return the result of serializing the elements of [[rootMargin]] space-separated, where pixel
    // lengths serialize as the numeric value followed by "px", and percentages serialize as the numeric value
    // followed by "%". Note that this is not guaranteed to be identical to the options.rootMargin passed to the
    // IntersectionObserver constructor. If no rootMargin was passed to the IntersectionObserver
    // constructor, the value of this attribute is "0px 0px 0px 0px".
    StringBuilder builder;
    builder.append(m_root_margin[0].to_string());
    builder.append(' ');
    builder.append(m_root_margin[1].to_string());
    builder.append(' ');
    builder.append(m_root_margin[2].to_string());
    builder.append(' ');
    builder.append(m_root_margin[3].to_string());
    return builder.to_string().value();
}

// https://w3c.github.io/IntersectionObserver/#dom-intersectionobserver-scrollmargin
String IntersectionObserver::scroll_margin() const
{
    // On getting, return the result of serializing the elements of [[scrollMargin]] space-separated, where pixel
    // lengths serialize as the numeric value followed by "px", and percentages serialize as the numeric value
    // followed by "%". Note that this is not guaranteed to be identical to the options.scrollMargin passed to the
    // IntersectionObserver constructor. If no scrollMargin was passed to the IntersectionObserver
    // constructor, the value of this attribute is "0px 0px 0px 0px".
    StringBuilder builder;
    builder.append(m_scroll_margin[0].to_string());
    builder.append(' ');
    builder.append(m_scroll_margin[1].to_string());
    builder.append(' ');
    builder.append(m_scroll_margin[2].to_string());
    builder.append(' ');
    builder.append(m_scroll_margin[3].to_string());
    return builder.to_string().value();
}

// https://www.w3.org/TR/intersection-observer/#intersectionobserver-intersection-root
Variant<GC::Root<DOM::Element>, GC::Root<DOM::Document>> IntersectionObserver::intersection_root() const
{
    // The intersection root for an IntersectionObserver is the value of its root attribute
    // if the attribute is non-null;
    if (m_root) {
        if (m_root->is_element())
            return GC::make_root(static_cast<DOM::Element&>(*m_root));
        if (m_root->is_document())
            return GC::make_root(static_cast<DOM::Document&>(*m_root));
        VERIFY_NOT_REACHED();
    }

    // otherwise, it is the top-level browsing context’s document node, referred to as the implicit root.
    return GC::make_root(as<HTML::Window>(HTML::relevant_global_object(*this)).page().top_level_browsing_context().active_document());
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
    if (intersection_root.has<GC::Root<DOM::Document>>()) {
        auto document = intersection_root.get<GC::Root<DOM::Document>>();

        // Since the spec says that this is only reach if the document is fully active, that means it must have a navigable.
        VERIFY(document->navigable());

        // NOTE: This rect is the *size* of the viewport. The viewport *offset* is not relevant,
        //       as intersections are computed using viewport-relative element rects.
        rect = CSSPixelRect { CSSPixelPoint { 0, 0 }, document->viewport_rect().size() };
    } else {
        VERIFY(intersection_root.has<GC::Root<DOM::Element>>());
        auto element = intersection_root.get<GC::Root<DOM::Element>>();

        // FIXME: Otherwise, if the intersection root has a content clip,
        //          it’s the element’s content area.

        // Otherwise,
        //    it’s the result of getting the bounding box for the intersection root.
        auto bounding_client_rect = element->get_bounding_client_rect();
        rect = CSSPixelRect(bounding_client_rect->x(), bounding_client_rect->y(), bounding_client_rect->width(), bounding_client_rect->height());
    }

    // When calculating the root intersection rectangle for a same-origin-domain target, the rectangle is then
    // expanded according to the offsets in the IntersectionObserver’s [[rootMargin]] slot in a manner similar
    // to CSS’s margin property, with the four values indicating the amount the top, right, bottom, and left
    // edges, respectively, are offset by, with positive lengths indicating an outward offset. Percentages
    // are resolved relative to the width of the undilated rectangle.
    DOM::Document* document = { nullptr };
    if (intersection_root.has<GC::Root<DOM::Document>>()) {
        document = intersection_root.get<GC::Root<DOM::Document>>().cell();
    } else {
        document = &intersection_root.get<GC::Root<DOM::Element>>().cell()->document();
    }
    if (m_document.has_value() && document->origin().is_same_origin(m_document->origin())) {
        auto layout_node = intersection_root.visit([&](auto& elem) { return static_cast<GC::Root<DOM::Node>>(*elem)->layout_node(); });
        rect.inflate(
            m_root_margin[0].to_px(*layout_node, rect.height()),
            m_root_margin[1].to_px(*layout_node, rect.width()),
            m_root_margin[2].to_px(*layout_node, rect.height()),
            m_root_margin[3].to_px(*layout_node, rect.width()));
    }

    return rect;
}

void IntersectionObserver::queue_entry(Badge<DOM::Document>, GC::Ref<IntersectionObserverEntry> entry)
{
    m_queued_entries.append(entry);
}

// https://w3c.github.io/IntersectionObserver/#parse-a-margin
Optional<Vector<CSS::LengthPercentage>> IntersectionObserver::parse_a_margin(JS::Realm& realm, String margin_string)
{
    // 1. Parse a list of component values marginString, storing the result as tokens.
    auto tokens = CSS::Parser::Parser::create(CSS::Parser::ParsingParams { realm }, margin_string).parse_as_list_of_component_values();

    // 2. Remove all whitespace tokens from tokens.
    tokens.remove_all_matching([](auto componentValue) { return componentValue.is(CSS::Parser::Token::Type::Whitespace); });

    // 3. If the length of tokens is greater than 4, return failure.
    if (tokens.size() > 4) {
        return {};
    }

    // 4. If there are zero elements in tokens, set tokens to ["0px"].
    if (tokens.size() == 0) {
        tokens.append(CSS::Parser::Token::create_dimension(0, "px"_fly_string));
    }

    // 5. Replace each token in tokens:
    // NOTE: In the spec, tokens miraculously changes type from a list of component values
    //       to a list of pixel lengths or percentages.
    Vector<CSS::LengthPercentage> tokens_length_percentage;
    for (auto const& token : tokens) {
        // If token is an absolute length dimension token, replace it with a an equivalent pixel length.
        if (token.is(CSS::Parser::Token::Type::Dimension)) {
            auto length = CSS::Length(token.token().dimension_value(), CSS::Length::unit_from_name(token.token().dimension_unit()).value());
            if (length.is_absolute()) {
                length.absolute_length_to_px();
                tokens_length_percentage.append(length);
                continue;
            }
        }
        // If token is a <percentage> token, replace it with an equivalent percentage.
        if (token.is(CSS::Parser::Token::Type::Percentage)) {
            tokens_length_percentage.append(CSS::Percentage(token.token().percentage()));
            continue;
        }
        // Otherwise, return failure.
        return {};
    }

    // 6.
    switch (tokens_length_percentage.size()) {
    // If there is one element in tokens, append three duplicates of that element to tokens.
    case 1:
        tokens_length_percentage.append(tokens_length_percentage.first());
        tokens_length_percentage.append(tokens_length_percentage.first());
        tokens_length_percentage.append(tokens_length_percentage.first());
        break;
    // Otherwise, if there are two elements are tokens, append a duplicate of each element to tokens.
    case 2:
        tokens_length_percentage.append(tokens_length_percentage.at(0));
        tokens_length_percentage.append(tokens_length_percentage.at(1));
        break;
    // Otherwise, if there are three elements in tokens, append a duplicate of the second element to tokens.
    case 3:
        tokens_length_percentage.append(tokens_length_percentage.at(1));
        break;
    }

    // 7. Return tokens.
    return tokens_length_percentage;
}

}
