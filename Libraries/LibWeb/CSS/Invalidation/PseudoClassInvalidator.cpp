/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/StdLibExtras.h>
#include <AK/TemporaryChange.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS::Invalidation {

enum class RequireAffectedByPseudoClassMetadata : u8 {
    Yes,
    No,
};

static bool pseudo_class_state_can_be_observed_across_style_scopes(CSS::PseudoClass pseudo_class)
{
    return first_is_one_of(pseudo_class, CSS::PseudoClass::Focus, CSS::PseudoClass::FocusWithin, CSS::PseudoClass::FocusVisible);
}

static CSS::StyleScope& style_scope_for_invalidation_root(DOM::Document& document, DOM::Node& invalidation_root)
{
    auto& root = invalidation_root.root();
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(root))
        return shadow_root->style_scope();
    return document.style_scope();
}

static GC::Ptr<DOM::Element const> shadow_host_for_rule_matching(DOM::Element const& element, GC::Ptr<DOM::ShadowRoot const> rule_shadow_root)
{
    auto const& root_node = element.root();
    auto shadow_root = as_if<DOM::ShadowRoot>(root_node);
    auto element_shadow_root = element.shadow_root();

    GC::Ptr<DOM::Element const> shadow_host;
    if (element_shadow_root)
        shadow_host = element;
    else if (shadow_root)
        shadow_host = shadow_root->host();

    if (element.is_shadow_host() && rule_shadow_root != element.shadow_root())
        shadow_host = rule_shadow_root ? rule_shadow_root->host() : nullptr;

    return shadow_host;
}

template<typename StateSlot, typename NewState>
static void invalidate_style_after_pseudo_class_state_change_in_style_scope(CSS::PseudoClass pseudo_class, DOM::Document& document, StateSlot& state_slot, DOM::Node& invalidation_root, NewState new_state, CSS::StyleScope& style_scope, RequireAffectedByPseudoClassMetadata require_metadata)
{
    auto rule_shadow_root = as_if<DOM::ShadowRoot>(style_scope.node());

    auto const& rules = style_scope.get_pseudo_class_rule_cache(pseudo_class);

    auto& style_computer = document.style_computer();
    auto does_rule_match_on_element = [&](DOM::Element const& element, CSS::MatchingRule const& rule) {
        auto const& selector = rule.selector;
        if (selector.can_use_ancestor_filter() && style_computer.should_reject_with_ancestor_filter(selector))
            return false;

        SelectorEngine::MatchContext context {
            .style_sheet_for_rule = *rule.sheet,
            .subject = element,
            .rule_shadow_root = rule_shadow_root,
        };
        auto target_pseudo = selector.target_pseudo_element();
        return SelectorEngine::matches(selector, { element, target_pseudo }, shadow_host_for_rule_matching(element, rule_shadow_root), context);
    };

    auto matches_different_set_of_rules_after_state_change = [&](DOM::Element& element) {
        bool result = false;
        auto check_matching_rules = [&](auto const& matching_rules) {
            for (auto& rule : matching_rules) {
                bool before = does_rule_match_on_element(element, rule);
                TemporaryChange change { state_slot, new_state };
                bool after = does_rule_match_on_element(element, rule);
                if (before != after) {
                    result = true;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        };

        auto check_abstract_element = [&](DOM::AbstractElement abstract_element) {
            rules.for_each_matching_rules(abstract_element, [&](auto const& matching_rules) {
                return check_matching_rules(matching_rules);
            });
        };

        check_abstract_element({ element });

        for (u8 i = 0; !result && i < to_underlying(CSS::PseudoElement::KnownPseudoElementCount); ++i) {
            auto pseudo_element = static_cast<CSS::PseudoElement>(i);
            check_abstract_element({ element, pseudo_element });
        }

        if (!result)
            (void)check_matching_rules(rules.slotted_rules);
        if (!result)
            (void)check_matching_rules(rules.part_rules);

        return result;
    };

    auto should_check_element = [&](DOM::Element const& element) {
        if (require_metadata == RequireAffectedByPseudoClassMetadata::No)
            return true;
        return element.affected_by_pseudo_class(pseudo_class);
    };

    Function<void(DOM::Node&)> invalidate_affected_elements_recursively = [&](DOM::Node& node) -> void {
        if (node.is_element()) {
            auto& element = static_cast<DOM::Element&>(node);
            style_computer.push_ancestor(element);
            if (should_check_element(element) && matches_different_set_of_rules_after_state_change(element))
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
    for (auto* ancestor = invalidation_root.parent_or_shadow_host_element(); ancestor; ancestor = ancestor->parent_or_shadow_host_element())
        style_computer.push_ancestor(*ancestor);

    invalidate_affected_elements_recursively(invalidation_root);

    for (auto* ancestor = invalidation_root.parent_or_shadow_host_element(); ancestor; ancestor = ancestor->parent_or_shadow_host_element())
        style_computer.pop_ancestor(*ancestor);
}

template<typename StateSlot, typename NewState>
static void invalidate_style_after_pseudo_class_state_change_impl(CSS::PseudoClass pseudo_class, DOM::Document& document, StateSlot& state_slot, DOM::Node& invalidation_root, NewState new_state)
{
    auto& root_style_scope = style_scope_for_invalidation_root(document, invalidation_root);
    invalidate_style_after_pseudo_class_state_change_in_style_scope(pseudo_class, document, state_slot, invalidation_root, new_state, root_style_scope, RequireAffectedByPseudoClassMetadata::Yes);

    if (!pseudo_class_state_can_be_observed_across_style_scopes(pseudo_class))
        return;

    Vector<CSS::StyleScope*, 4> observer_style_scopes;
    auto append_observer_style_scopes = [&](auto node) {
        if (!node)
            return;
        node->for_each_style_scope_which_may_observe_the_node([&](CSS::StyleScope& style_scope) {
            if (!observer_style_scopes.contains_slow(&style_scope))
                observer_style_scopes.append(&style_scope);
        });
    };
    append_observer_style_scopes(state_slot);
    append_observer_style_scopes(new_state);

    Vector<DOM::ShadowRoot*, 4> part_invalidation_roots;
    auto append_part_invalidation_roots = [&](auto node) {
        if (!node)
            return;
        for (auto shadow_root = node->containing_shadow_root(); shadow_root; shadow_root = shadow_root->containing_shadow_root()) {
            if (!part_invalidation_roots.contains_slow(shadow_root.ptr()))
                part_invalidation_roots.append(shadow_root.ptr());
        }
    };
    append_part_invalidation_roots(state_slot);
    append_part_invalidation_roots(new_state);

    for (auto* observer_style_scope : observer_style_scopes) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(observer_style_scope->node())) {
            if (observer_style_scope != &root_style_scope || &invalidation_root != shadow_root)
                invalidate_style_after_pseudo_class_state_change_in_style_scope(pseudo_class, document, state_slot, *shadow_root, new_state, *observer_style_scope, RequireAffectedByPseudoClassMetadata::No);
            if (auto* host = shadow_root->host())
                invalidate_style_after_pseudo_class_state_change_in_style_scope(pseudo_class, document, state_slot, *host, new_state, *observer_style_scope, RequireAffectedByPseudoClassMetadata::No);
        } else if (observer_style_scope != &root_style_scope) {
            invalidate_style_after_pseudo_class_state_change_in_style_scope(pseudo_class, document, state_slot, observer_style_scope->node(), new_state, *observer_style_scope, RequireAffectedByPseudoClassMetadata::No);
        }

        if (observer_style_scope->get_pseudo_class_rule_cache(pseudo_class).part_rules.is_empty())
            continue;
        for (auto* part_invalidation_root : part_invalidation_roots)
            invalidate_style_after_pseudo_class_state_change_in_style_scope(pseudo_class, document, state_slot, *part_invalidation_root, new_state, *observer_style_scope, RequireAffectedByPseudoClassMetadata::No);
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
