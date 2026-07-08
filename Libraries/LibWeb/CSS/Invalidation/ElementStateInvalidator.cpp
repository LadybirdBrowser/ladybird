/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/ElementStateInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::CSS::Invalidation {

static void invalidate_style_after_pseudo_class_state_change(DOM::Element& element, DOM::StyleInvalidationReason reason,
    PseudoClass pseudo_class)
{
    // NB: Rules observing this pseudo-class through :has() are handled by the property-based invalidation path, which
    //     consults the :has() metadata recorded for feature pseudo-classes. Both :active and :open are feature
    //     pseudo-classes, so no coarse fallback is needed here.
    Vector<InvalidationSet::Property, 1> pseudo_class_properties {
        { .type = InvalidationSet::Property::Type::PseudoClass, .value = pseudo_class },
    };
    element.invalidate_style(reason, pseudo_class_properties, { .invalidate_self = true });
}

void invalidate_style_after_active_state_change(DOM::Element& element)
{
    invalidate_style_after_pseudo_class_state_change(element, DOM::StyleInvalidationReason::ElementSetActive,
        PseudoClass::Active);
}

void invalidate_style_after_modal_state_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::HTMLDialogElementSetIsModal);
}

void invalidate_style_after_open_state_change(DOM::Element& element)
{
    invalidate_style_after_pseudo_class_state_change(element,
        DOM::StyleInvalidationReason::HTMLDetailsOrDialogOpenAttributeChange, PseudoClass::Open);
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
