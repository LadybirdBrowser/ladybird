/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/ElementStateInvalidator.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_active_state_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::ElementSetActive);
}

void invalidate_style_after_modal_state_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::HTMLDialogElementSetIsModal);
}

void invalidate_style_after_open_state_change(DOM::Element& element)
{
    // The :open pseudo-class can affect sibling selectors (e.g. dialog:open + sibling), so keep the existing broad
    // subtree and sibling invalidation.
    element.invalidate_style(DOM::StyleInvalidationReason::HTMLDetailsOrDialogOpenAttributeChange);
}

void invalidate_style_after_option_selected_state_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::HTMLOptionElementSelectedChange);
}

void invalidate_style_after_input_open_state_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::HTMLInputElementSetIsOpen);
}

void invalidate_style_after_select_open_state_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::HTMLSelectElementSetIsOpen);
}

void invalidate_style_after_shadow_root_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::ElementSetShadowRoot);
}

}
