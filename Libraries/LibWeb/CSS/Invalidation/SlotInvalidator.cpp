/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/SlotInvalidator.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_slottable_assignment_change(DOM::Slottable const& slottable)
{
    slottable.visit(
        [](GC::Ref<DOM::Element> const& element) {
            element->set_needs_style_update(true);
        },
        [](GC::Ref<DOM::Text> const&) {});
}

}
