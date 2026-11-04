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

// Which groups of states an element's kind can be in, past those any element can. Elements of one name are of one kind,
// so this is the same answer for all of them, and asking the kinds they aren't of is what an element is spared.
enum class ElementStateQuestions : u8 {
    None = 0,
    Control = 1 << 0,
    Link = 1 << 1,
    Media = 1 << 2,
    Meter = 1 << 3,
};
AK_ENUM_BITWISE_OPERATORS(ElementStateQuestions);
WEB_API ElementStateQuestions element_state_questions(DOM::Element const&);

// The elements `:focus-within` holds on while this lives, as marks one walk up from the focused area leaves on them.
// Nothing may move focus or the flat tree meanwhile. An element then asks a mark rather than walking the chain.
class WEB_API FocusWithinChain {
    AK_MAKE_NONCOPYABLE(FocusWithinChain);
    AK_MAKE_NONMOVABLE(FocusWithinChain);

public:
    explicit FocusWithinChain(DOM::Document const&);
    ~FocusWithinChain();

    bool contains(DOM::Element const& element) const { return element.is_marked_on_focus_within_chain(); }

private:
    GC::Ref<DOM::Document const> m_document;
};

// Which of the boolean pseudo-classes an element is in right now, as a set.
//
// Asking `element_matches_state` once per pseudo-class recomputes what several of them share - an
// element's validity is the answer to four of them, its required state to two - and does it for
// every element that connects. This asks each shared question once and is the same answers.
//
// `:hover` is left out, as the engine's hover chain holds it.
//
// Whether a link is a local link is answered by parsing its URL, which is asked only where something reads it.
enum class AskLocalLink : u8 {
    No,
    Yes,
};
enum class AskValidity : u8 {
    No,
    Yes,
};
WEB_API CSS::PseudoClassBitmap element_states(DOM::Element const&, ElementStateQuestions, FocusWithinChain const&, AskLocalLink, AskValidity);
// Whether `:valid`, `:invalid`, `:user-valid` or `:user-invalid` can hold of the element.
WEB_API bool element_can_have_validity(DOM::Element const&);

}
