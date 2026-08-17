/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/StylePropertyMap.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>

namespace Web::DOM {

struct Element::RareData
    : Node::RareData
    , SlottableMixin::RareData
    , ARIA::ARIAMixin::RareData {
    virtual ~RareData() override;
    virtual void visit_edges(Cell::Visitor&) override;

    mutable Optional<Utf16FlyString> html_uppercased_qualified_name;
    GC::Ptr<CSS::StylePropertyMap> attribute_style_map;
    GC::Ptr<DOMTokenList> class_list;
    GC::Ptr<DOMTokenList> part_list;
    GC::Ptr<NamedNodeMap> attribute_map;
    mutable OwnPtr<PseudoElementData> pseudo_element_data;
    Optional<CSS::PseudoElement> associated_shadow_host_pseudo_element;
    Vector<Utf16FlyString> parts;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reaction-queue
    // All elements have an associated custom element reaction queue, initially empty. Each item in the custom element reaction queue is of one of two types:
    // NOTE: See the structs at the top of Element.h.
    OwnPtr<CustomElementReactionQueue> custom_element_reaction_queue;

    // https://dom.spec.whatwg.org/#concept-element-is-value
    Optional<Utf16FlyString> is_value;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#states-set
    GC::Ptr<HTML::CustomStateSet> custom_state_set;

    // https://www.w3.org/TR/intersection-observer/#dom-element-registeredintersectionobservers-slot
    // Element objects have an internal [[RegisteredIntersectionObservers]] slot, which is initialized to an empty list.
    OwnPtr<Vector<GC::Ref<IntersectionObserver::IntersectionObserver>>> registered_intersection_observers;

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemapcache-slot
    // Every Element has a [[computedStyleMapCache]] internal slot, initially set to null, which caches the result of
    // the computedStyleMap() method when it is first called.
    GC::Ptr<CSS::StylePropertyMapReadOnly> computed_style_map_cache;
    OwnPtr<CSS::CountersSet> counters_set;

    // https://drafts.csswg.org/css-values-5/#random-caching
    HashMap<CSS::RandomCachingKey, double> element_specific_css_random_base_value_cache;

    // https://dom.spec.whatwg.org/#concept-element-custom-element-definition
    GC::Ptr<HTML::CustomElementDefinition> custom_element_definition;

    // https://dom.spec.whatwg.org/#element-custom-element-registry
    GC::Ptr<HTML::CustomElementRegistry> custom_element_registry;

    // https://html.spec.whatwg.org/multipage/dom.html#dom-dataset-dev
    GC::Ptr<HTML::DOMStringMap> dataset;

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#cryptographicnonce
    Utf16String cryptographic_nonce;

    Optional<Utf16FlyString> name;
    Optional<Dir> dir;
    CSSPixelPoint scroll_offset;
    Fullscreen::RequestType fullscreen_request_type { Fullscreen::RequestType::Standard };

    // https://w3c.github.io/webappsec-csp/#is-element-nonceable
    // AD-HOC: We need to know the element had a duplicate attribute when it was created from the HTML parser.
    //         However, there currently isn't any specified way to do this, so we store a flag on the token, which is
    //         then passed down to here. This is used by Content Security Policy to disable the nonce attribute if this
    //         flag is set.
    bool had_duplicate_attribute_during_tokenization { false };

    // https://drafts.csswg.org/css-contain/#proximity-to-the-viewport
    ProximityToTheViewport proximity_to_the_viewport { ProximityToTheViewport::NotDetermined };

    // https://drafts.csswg.org/css-view-transitions-1/#captured-in-a-view-transition
    bool captured_in_a_view_transition { false };
};

}
