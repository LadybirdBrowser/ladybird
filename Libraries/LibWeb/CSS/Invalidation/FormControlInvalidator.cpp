/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_checked_state_change(DOM::Element& element, DOM::StyleInvalidationReason reason)
{
    element.invalidate_style(
        reason,
        {
            { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Checked },
            { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Unchecked },
        },
        {});
}

}
