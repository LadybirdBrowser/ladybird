/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS::Invalidation {

template<typename StateSlot, typename NewState>
static void invalidate_style_after_pseudo_class_state_change_impl(CSS::PseudoClass pseudo_class, DOM::Document& document, StateSlot& state_slot, DOM::Node& invalidation_root, NewState new_state)
{
    auto& root = invalidation_root.root();
    auto shadow_root = is<DOM::ShadowRoot>(root) ? static_cast<DOM::ShadowRoot const*>(&root) : nullptr;
    auto& style_scope = shadow_root ? shadow_root->style_scope() : document.style_scope();

    auto const& rules = style_scope.get_pseudo_class_rule_cache(pseudo_class);

    auto& style_computer = document.style_computer();
    auto does_rule_match_on_element = [&](DOM::Element const& element, CSS::MatchingRule const& rule) {
        auto const& selector = rule.selector;
        if (selector.can_use_ancestor_filter() && style_computer.should_reject_with_ancestor_filter(selector))
            return false;

        SelectorEngine::MatchContext context;
        auto const& target_pseudo = selector.target_pseudo_element();
        if (!target_pseudo.has_value())
            return SelectorEngine::matches(selector, element, {}, context);
        switch (target_pseudo->type()) {
        case CSS::PseudoElement::Before:
            return SelectorEngine::matches(selector, { element, CSS::PseudoElement::Before }, {}, context);
        case CSS::PseudoElement::After:
            return SelectorEngine::matches(selector, { element, CSS::PseudoElement::After }, {}, context);
        default:
            return false;
        }
    };

    auto matches_different_set_of_rules_after_state_change = [&](DOM::Element& element) {
        bool result = false;
        rules.for_each_matching_rules({ element }, [&](auto& rules) {
            for (auto& rule : rules) {
                bool before = does_rule_match_on_element(element, rule);
                TemporaryChange change { state_slot, new_state };
                bool after = does_rule_match_on_element(element, rule);
                if (before != after) {
                    result = true;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        });
        return result;
    };

    Function<void(DOM::Node&)> invalidate_affected_elements_recursively = [&](DOM::Node& node) -> void {
        if (node.is_element()) {
            auto& element = static_cast<DOM::Element&>(node);
            style_computer.push_ancestor(element);
            if (element.affected_by_pseudo_class(pseudo_class) && matches_different_set_of_rules_after_state_change(element))
                element.set_needs_style_update(true);
        }

        node.for_each_child([&](auto& child) {
            invalidate_affected_elements_recursively(child);
            return IterationDecision::Continue;
        });

        if (node.is_element())
            style_computer.pop_ancestor(static_cast<DOM::Element&>(node));
    };

    // Seed the ancestor filter with ancestors above the starting node,
    // so that ancestor-dependent selectors can still be correctly rejected.
    for (auto* ancestor = invalidation_root.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->is_element())
            style_computer.push_ancestor(static_cast<DOM::Element&>(*ancestor));
    }

    invalidate_affected_elements_recursively(invalidation_root);

    for (auto* ancestor = invalidation_root.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->is_element())
            style_computer.pop_ancestor(static_cast<DOM::Element&>(*ancestor));
    }
}

void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass pseudo_class, DOM::Document& document, GC::Ptr<DOM::Node>& state_slot, DOM::Node& invalidation_root, GC::Ptr<DOM::Node> new_state)
{
    invalidate_style_after_pseudo_class_state_change_impl(pseudo_class, document, state_slot, invalidation_root, new_state);
}

void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass pseudo_class, DOM::Document& document, GC::Ptr<DOM::Element>& state_slot, DOM::Node& invalidation_root, GC::Ptr<DOM::Element> new_state)
{
    invalidate_style_after_pseudo_class_state_change_impl(pseudo_class, document, state_slot, invalidation_root, new_state);
}

}
