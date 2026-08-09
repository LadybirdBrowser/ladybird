/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PseudoClassBitmap.h>
#include <LibWeb/DOM/Element.h>

namespace Web::SelectorMatching {

// Whether an element satisfies its constraints, for `:valid`/`:invalid` and their user-interacted
// counterparts. Shared so that invalidation publishes the same truth matching will read.
enum class ValidityState : u8 {
    NotApplicable,
    Valid,
    Invalid,
};

WEB_API ValidityState element_validity_state(DOM::Element const&);
WEB_API ValidityState element_user_validity_state(DOM::Element const&);

// Whether `:empty` would match an element as it would be without one of its children: it has no
// other element children, and no other text child holding anything. An empty text node leaves the
// element empty, which is why the child count cannot answer this. Ignoring a child is what a child
// arriving, leaving, or changing its data needs to compare against.
WEB_API bool element_is_empty_ignoring_child(DOM::Element const&, DOM::Node const& ignored_child);

// Whether `:required` or `:optional` applies to an element, or neither.
enum class RequiredState : u8 {
    NotApplicable,
    Required,
    Optional,
};

WEB_API RequiredState element_required_state(DOM::Element const&);

// Whether one boolean pseudo-class holds on an element right now.
//
// The switch behind this is exhaustive over PseudoClass, so a pseudo-class cannot be added without
// deciding whether it is a fact an element carries. That matters because StyleEngine publishes these
// as facts when an element connects: a state nothing answers for is a state no selector resting on
// it can ever match, which is silent rather than loud.
WEB_API bool element_matches_state(DOM::Element const&, CSS::PseudoClass);

// Which of the boolean pseudo-classes an element is in right now, as a set.
//
// Asking `element_matches_state` once per pseudo-class recomputes what several of them share - an
// element's validity is the answer to four of them, its required state to two - and does it for
// every element that connects. This asks each shared question once and is the same answers.
WEB_API CSS::PseudoClassBitmap element_states(DOM::Element const&);

}
