/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>

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

void invalidate_style_after_validity_change(DOM::Element& element)
{
    auto invalidate = [](DOM::Element& element_to_invalidate) {
        element_to_invalidate.invalidate_style(
            DOM::StyleInvalidationReason::FormControlValidityChange,
            {
                { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Valid },
                { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Invalid },
                { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::UserValid },
                { .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::UserInvalid },
            },
            {});
    };

    invalidate(element);

    if (auto* html_element = as_if<HTML::HTMLElement>(element)) {
        if (auto form = html_element->form())
            invalidate(*form);
    }
    for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
        if (is<HTML::HTMLFieldSetElement>(*ancestor))
            invalidate(*ancestor);
    }
}

}
