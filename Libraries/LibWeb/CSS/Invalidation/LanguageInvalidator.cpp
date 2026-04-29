/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/TraversalDecision.h>

namespace Web::CSS::Invalidation {

static void invalidate_descendants_affected_by_language_or_directionality(DOM::Element& element, bool is_directionality_change)
{
    // dir and lang both inherit, so all descendants' :dir() / :lang() matches and direction-dependent layout/text
    // need to be recomputed.
    element.for_each_shadow_including_inclusive_descendant([is_directionality_change](auto& node) {
        if (auto* element = as_if<DOM::Element>(node)) {
            if (!is_directionality_change)
                element->invalidate_lang_value();
            element->set_needs_style_update(true);
        }
        return TraversalDecision::Continue;
    });

    // :has(:dir(...)) and :has(:lang(...)) on ancestors aren't keyed on any property the regular invalidation
    // plan tracks, so explicitly schedule the :has() ancestor walk here.
    element.for_each_style_scope_which_may_observe_the_node([&](CSS::StyleScope& scope) {
        scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(element);
    });
}

void invalidate_style_after_language_change(DOM::Element& element)
{
    invalidate_descendants_affected_by_language_or_directionality(element, false);
}

void invalidate_style_after_directionality_change(DOM::Element& element)
{
    invalidate_descendants_affected_by_language_or_directionality(element, true);
}

}
