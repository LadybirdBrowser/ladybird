/*
 * Copyright (c) 2024, circl <circl.lastname@gmail.com>
 * Copyright (c) 2025, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/Animatable.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Utils.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::DOM {

template<typename T>
concept DocumentOrShadowRoot = OneOf<T, Document, ShadowRoot>;

// https://html.spec.whatwg.org/multipage/interaction.html#dom-documentorshadowroot-activeelement
template<DocumentOrShadowRoot T>
GC::Ptr<Element> calculate_active_element(T& self)
{
    // 1. Let candidate be this's node document's focused area's DOM anchor.
    Node* candidate = self.document().focused_area();

    // AD-HOC: null focused_area indicates "viewport focus".
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // If the focusable area is the viewport of a Document that has a non-null browsing context and is not inert then
    // the DOM anchor is the document for which the viewport was created.
    if (!candidate && self.document().browsing_context() && !self.document().is_inert())
        candidate = &self.document();

    // 2. Set candidate to the result of retargeting candidate against this.
    candidate = as<Node>(retarget(candidate, &self));

    // 3. If candidate's root is not this, then return null.
    if (!candidate || &candidate->root() != &self)
        return nullptr;

    // 4. If candidate is not a Document object, then return candidate.
    if (!is<Document>(candidate))
        return as<Element>(candidate);

    auto* candidate_document = as_if<Document>(candidate);

    // 5. If candidate has a body element, then return that body element.
    if (auto* body = candidate_document->body())
        return body;

    // 6. If candidate's document element is non-null, then return that document element.
    if (auto* document_element = candidate_document->document_element())
        return document_element;

    // 7. Return null.
    return nullptr;
}

// https://drafts.csswg.org/web-animations-1/#dom-documentorshadowroot-getanimations
template<DocumentOrShadowRoot T>
WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> calculate_get_animations(T& self)
{
    // Returns the set of relevant animations for a subtree for the document or shadow root on which this
    // method is called.
    Vector<GC::Ref<Animations::Animation>> relevant_animations;
    TRY(self.template for_each_child_of_type_fallible<Element>([&](auto& child) -> WebIDL::ExceptionOr<IterationDecision> {
        relevant_animations.extend(TRY(child.get_animations_internal(
            Animations::Animatable::GetAnimationsSorted::No,
            Animations::GetAnimationsOptions { .subtree = true })));
        return IterationDecision::Continue;
    }));

    // The returned list is sorted using the composite order described for the associated animations of
    // effects in § 5.4.2 The effect stack.
    quick_sort(relevant_animations, [](GC::Ref<Animations::Animation>& a, GC::Ref<Animations::Animation>& b) {
        auto& a_effect = as<Animations::KeyframeEffect>(*a->effect());
        auto& b_effect = as<Animations::KeyframeEffect>(*b->effect());
        return Animations::KeyframeEffect::composite_order(a_effect, b_effect) < 0;
    });

    // Calling this method triggers a style change event for the document. As a result, the returned list
    // reflects the state after applying any pending style changes to animation such as changes to
    // animation-related style properties that have yet to be processed.
    // FIXME: Implement this.

    return relevant_animations;
}

}
