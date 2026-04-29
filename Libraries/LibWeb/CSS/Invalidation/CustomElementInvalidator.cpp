/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/CustomElementInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_custom_element_state_change(DOM::Element& element)
{
    element.invalidate_style(
        DOM::StyleInvalidationReason::CustomElementStateChange,
        {
            { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Defined },
        },
        {});
}

void invalidate_style_after_custom_state_set_change(DOM::Element& element)
{
    element.invalidate_style(DOM::StyleInvalidationReason::CustomStateSetChange);
}

}
