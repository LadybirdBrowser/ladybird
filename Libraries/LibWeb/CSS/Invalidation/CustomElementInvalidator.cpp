/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/CustomElementInvalidator.h>
#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_custom_element_state_change(DOM::Element& element)
{
    record_element_state_changed(element, PseudoClass::Defined, element.is_defined());
    record_element_state_changed(element, PseudoClass::Enabled, SelectorMatching::element_matches_state(element, PseudoClass::Enabled));
    record_element_state_changed(element, PseudoClass::Disabled, SelectorMatching::element_matches_state(element, PseudoClass::Disabled));
    invalidate_style_after_validity_change(element);
}

void invalidate_style_after_custom_state_set_change(DOM::Element& element)
{
    record_element_custom_states_changed(element);
}

}
