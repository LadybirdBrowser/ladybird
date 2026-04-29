/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/LinkInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_hyperlink_state_change(DOM::Element& element)
{
    element.invalidate_style(
        DOM::StyleInvalidationReason::HTMLHyperlinkElementHrefChange,
        {
            { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::AnyLink },
            { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Link },
            { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::LocalLink },
        },
        {});
}

}
