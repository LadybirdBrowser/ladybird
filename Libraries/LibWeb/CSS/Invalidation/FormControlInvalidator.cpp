/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_checked_state_change(DOM::Element& element)
{
    record_element_state_changed(element, PseudoClass::Checked, element.matches_checked_pseudo_class());
    record_element_state_changed(element, PseudoClass::Unchecked, element.matches_unchecked_pseudo_class());
}

// A control's value decides whether its placeholder shows, and a value can move without an
// attribute moving with it: a textarea with no dirty value flag takes its value from its children,
// and script and the user both edit values in place. Only the attribute path published this state,
// so `:placeholder-shown` was left to whatever the structural invalidation happened to reach.
void invalidate_style_after_placeholder_shown_change(DOM::Element& element)
{
    record_element_state_changed(element, PseudoClass::PlaceholderShown, element.matches_placeholder_shown_pseudo_class());
}

// A control's validity is also its form's and its ancestor fieldsets': `:invalid` on a form is a
// statement about the controls under it. Each element whose truth can have moved hears the state it
// now has, which is what lets a change that moved nothing publish nothing.
static void record_validity_states(DOM::Element& element)
{
    auto validity = SelectorMatching::element_validity_state(element);
    record_element_state_changed(element, PseudoClass::Valid, validity == SelectorMatching::ValidityState::Valid);
    record_element_state_changed(element, PseudoClass::Invalid, validity == SelectorMatching::ValidityState::Invalid);

    auto user_validity = SelectorMatching::element_user_validity_state(element);
    record_element_state_changed(element, PseudoClass::UserValid, user_validity == SelectorMatching::ValidityState::Valid);
    record_element_state_changed(element, PseudoClass::UserInvalid, user_validity == SelectorMatching::ValidityState::Invalid);
}

void invalidate_style_after_form_control_left(DOM::Node& old_ancestor)
{
    for (GC::Ptr<DOM::Element> ancestor = as_if<DOM::Element>(old_ancestor); ancestor; ancestor = ancestor->parent_element()) {
        if (is<HTML::HTMLFormElement>(*ancestor) || is<HTML::HTMLFieldSetElement>(*ancestor))
            record_validity_states(*ancestor);
    }
}

void invalidate_style_after_validity_change(DOM::Element& element)
{
    auto visit = [&](DOM::Element& element_to_invalidate) {
        record_validity_states(element_to_invalidate);
    };

    visit(element);

    if (auto* html_element = as_if<HTML::HTMLElement>(element)) {
        if (auto form = html_element->form())
            visit(*form);
    }
    for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
        if (is<HTML::HTMLFieldSetElement>(*ancestor))
            visit(*ancestor);
    }
}

}
